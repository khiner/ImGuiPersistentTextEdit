#include "Colorize.h"
#include "Cursors.h"
#include "Document.h"
#include "Layout.h"
#include "TextEdit.h"
#include "Update.h"

#include "imgui_internal.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <utility>

namespace ipte {
namespace {
ImU32 Shade(const State &s, Color c) { return s.Shaded[size_t(c)]; }

struct Metrics {
    ImDrawList *Draw;
    ImFont *Font;
    float FontSize, FontScaleDpi;
    ImVec2 GlyphSize, CursorScreenPos, TextSize;
    float LineNumberLeftOffset, LineNumberRightOffset, FoldIndicatorOffset, TextLeftOffset, TextRightOffset, MiniMapWidth;
    float MiniMapRowHeight, MiniMapColumnHeight, MiniMapColumnWidth;
};
constexpr float MiniMapAlpha = 0.45f, MiniMapViewPortAlpha = 0.15f, MiniMapViewPortActiveAlpha = 0.3f;

// Returns the screen position of the glyph at `p`.
ImVec2 ScreenPos(const Metrics &m, VisPos p) { return {m.CursorScreenPos.x + m.TextLeftOffset + p.Column * m.GlyphSize.x, m.CursorScreenPos.y + p.Row * m.GlyphSize.y}; }

// Returns `color` with its alpha scaled by `alpha`.
ImU32 ScaleAlpha(ImU32 color, float alpha) {
    auto c = ImGui::ColorConvertU32ToFloat4(color);
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

void FlushScroll(State &s) {
    if (s.ScrollTarget.x < FLT_MAX) ImGui::SetScrollX(s.ScrollTarget.x);
    if (s.ScrollTarget.y < FLT_MAX) ImGui::SetScrollY(s.ScrollTarget.y);
    s.ScrollTarget = ImVec2(FLT_MAX, FLT_MAX);
}

// Keeps the items `index` marks and sets each mark to its item's new id.
template<typename T> void Compact(std::vector<T> &items, std::vector<uint64_t> &index) {
    uint64_t next = 1;
    for (auto &i : index)
        if (i) i = next++;
    std::vector<T> kept;
    for (size_t i = 0; i < items.size(); ++i)
        if (index[i]) kept.push_back(items[i]);
    items = std::move(kept);
}

void CompressMarkers(State &s) {
    if (s.Markers.empty()) return;
    std::vector<uint64_t> index(s.Markers.size(), 0);
    for (const auto m : s.LineMarkers)
        if (m) index[m - 1] = 1;
    Compact(s.Markers, index);
    for (auto &m : s.LineMarkers)
        if (m) m = index[m - 1];
}

void CompressSquiggles(State &s) {
    if (s.Squiggles.empty()) return;
    std::vector<uint64_t> index(s.Squiggles.size(), 0);
    vec::ForEach(s.GlyphSquiggles, [&](uint64_t id) {
        if (id) index[id - 1] = 1;
    });
    Compact(s.Squiggles, index);
    auto t = s.GlyphSquiggles.transient();
    for (uint64_t i = 0; i < s.GlyphSquiggles.Size; ++i)
        if (const auto id = s.GlyphSquiggles[i]) t.set(i, index[id - 1]);
    s.GlyphSquiggles = std::move(t).persistent();
}

uint64_t MarkerAt(const State &s, uint64_t line) { return line < s.LineMarkers.size() ? s.LineMarkers[line] : 0; }
uint64_t SquiggleAt(const State &s, uint64_t offset) { return offset < s.GlyphSquiggles.Size ? s.GlyphSquiggles[offset] : 0; }

// Calls `f(row, marker)` for each row in [first, last) whose line has a marker.
void ForEachMarker(const State &s, uint64_t first, uint64_t last, auto &&f) {
    if (s.Markers.empty()) return;
    for (auto row = first; row < last; ++row)
        if (const auto i = MarkerAt(s, s.Layout.Rows[row].Line)) f(row, s.Markers[i - 1]);
}

// Calls `f(line)` with the layout of each visible line from a cursor's selection start to its end.
void ForEachSelectedLine(const State &s, auto &&f) {
    for (const auto &c : s.Cursors.Items) {
        const auto begin = c.SelectionStart().Line, end = c.SelectionEnd().Line;
        for (auto i = begin; i <= end; ++i)
            if (!IsHidden(s, i)) f(LineAt(s, i));
    }
}

void UpdateState(State &s, const Metrics &m) {
    const auto bracketsChanged = Refresh(s);
    const auto foldsUpdated = RefreshFolds(s, bracketsChanged);
    UpdateCursors(s);

    float unused;
    const auto firstVisibleLineFraction = std::modf(s.Scroll.y / m.GlyphSize.y, &unused);
    const auto previousFirstLine = VisToDoc(s, {s.FirstVisibleRow, 0}).Line;
    const bool configChanged = s.Layout.TabSize != s.Config.TabSize || s.Layout.WordWrap != s.Config.WordWrap || (s.Layout.WordWrap && s.Layout.WordWrapColumns != s.WordWrapColumns);
    const bool scrollRequested = s.ScrollToLineNumber != InvalidLine || s.ScrollToDocPos.Line != InvalidLine;
    if ((configChanged || s.DocUpdated || foldsUpdated) && !scrollRequested) ScrollToLine(s, previousFirstLine, State::Align::Top, firstVisibleLineFraction);
    RefreshLayout(s);

    s.DocUpdated = false;
    if (std::exchange(s.DeletesHappened, false)) {
        CompressMarkers(s);
        CompressSquiggles(s);
    }

    RefreshMiniMap(s);

    s.TotalSize = ImVec2(m.TextLeftOffset + s.Layout.TotalColumns * m.GlyphSize.x + s.CursorWidth + m.MiniMapWidth, RowCount(s) * m.GlyphSize.y);
}

void HandlePossibleScrolling(State &s, const Metrics &m) {
    float scrollX = -1.0f, scrollY = -1.0f;
    if (s.ScrollToDocPos.Line != InvalidLine) {
        if (s.LastVisibleRow == 0 && s.LastVisibleColumn == 0) {
            s.LastVisibleRow = std::min(uint64_t(std::ceil(m.TextSize.y / m.GlyphSize.y)), RowCount(s) - 1);
            s.LastVisibleColumn = uint64_t(std::ceil(m.TextSize.x / m.GlyphSize.x));
        }
        const auto pos = DocToVis(s, Normalize(s.Doc, s.ScrollToDocPos));
        if (pos.Row <= s.FirstVisibleRow + 1) scrollY = std::max(0.0f, (pos.Row - 2.0f) * m.GlyphSize.y);
        else if (pos.Row >= s.LastVisibleRow - 1) scrollY = std::max(0.0f, (pos.Row + 2.0f) * m.GlyphSize.y - m.TextSize.y);
        if (pos.Column <= s.FirstVisibleColumn + 1) scrollX = std::max(0.0f, (pos.Column - 2.0f) * m.GlyphSize.x);
        else if (pos.Column >= s.LastVisibleColumn - 1) scrollX = std::max(0.0f, (pos.Column + 2.0f) * m.GlyphSize.x - m.TextSize.x);
        s.ScrollToDocPos.Line = InvalidLine;
    }
    if (s.ScrollToLineNumber != InvalidLine) {
        const auto row = float(DocToVis(s, {NormalizeLine(s.Doc, s.ScrollToLineNumber), 0}).Row);
        const auto visibleRows = m.TextSize.y / m.GlyphSize.y;
        scrollX = 0.0f;
        switch (s.ScrollToAlignment) {
            case State::Align::Top: scrollY = (row + s.ScrollToFraction) * m.GlyphSize.y; break;
            case State::Align::Middle: scrollY = std::max(0.0f, (row - visibleRows / 2.0f) * m.GlyphSize.y); break;
            case State::Align::Bottom: scrollY = std::max(0.0f, (row - (visibleRows - 1.0f)) * m.GlyphSize.y); break;
        }
        s.ScrollToLineNumber = InvalidLine;
    }
    if (scrollX >= 0.0f) s.ScrollTarget.x = scrollX;
    if (scrollY >= 0.0f) s.ScrollTarget.y = scrollY;
    FlushScroll(s);
}

void Apply(State &s, const Action &a) {
    s = Update(std::move(s), a);
    FlushScroll(s);
    if (std::exchange(s.SetClipboard, false)) {
        ImGui::SetClipboardText(s.ClipboardOut.c_str());
        s.ClipboardOut.clear();
    }
}

// Checks shortcuts in upstream's order. Returns the action of the first one pressed.
std::optional<Action> KeyboardAction(const State &s) {
    const auto macOS = ImGui::GetIO().ConfigMacOSXBehaviors;
    const auto word = macOS ? ImGuiMod_Alt : ImGuiMod_Ctrl;
    const auto selection = macOS ? ImGuiMod_Super : ImGuiMod_Alt;
    const auto ro = s.Config.ReadOnly;
    const auto hasSelection = CurrentCursor(s.Cursors).HasSelection();
    const auto page = s.LastVisibleRow - s.FirstVisibleRow - 2;
    const auto sc = [](ImGuiKeyChord chord, ImGuiInputFlags flags = 0) { return ImGui::Shortcut(chord, flags); };
    constexpr auto R = ImGuiInputFlags_Repeat;

    if (sc(ImGuiKey_UpArrow, R)) return Action{Op::MoveUp, false, false, 1};
    if (sc(ImGuiMod_Shift | ImGuiKey_UpArrow, R)) return Action{Op::MoveUp, true, false, 1};
    if (sc(ImGuiKey_DownArrow, R)) return Action{Op::MoveDown, false, false, 1};
    if (sc(ImGuiMod_Shift | ImGuiKey_DownArrow, R)) return Action{Op::MoveDown, true, false, 1};

    if (sc(selection | ImGuiMod_Shift | ImGuiKey_LeftArrow)) return Action{Op::ShrinkSelections};
    if (sc(selection | ImGuiMod_Shift | ImGuiKey_RightArrow)) return Action{Op::GrowSelections};

    if (sc(ImGuiKey_LeftArrow, R)) return Action{Op::MoveLeft, false, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_LeftArrow, R)) return Action{Op::MoveLeft, true, false};
    if (sc(word | ImGuiKey_LeftArrow, R)) return Action{Op::MoveLeft, false, true};
    if (sc(word | ImGuiMod_Shift | ImGuiKey_LeftArrow, R)) return Action{Op::MoveLeft, true, true};

    if (sc(ImGuiKey_RightArrow, R)) return Action{Op::MoveRight, false, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_RightArrow, R)) return Action{Op::MoveRight, true, false};
    if (sc(word | ImGuiKey_RightArrow, R)) return Action{Op::MoveRight, false, true};
    if (sc(word | ImGuiMod_Shift | ImGuiKey_RightArrow, R)) return Action{Op::MoveRight, true, true};

    if (sc(ImGuiKey_PageUp, R)) return Action{Op::MoveUp, false, false, page};
    if (sc(ImGuiMod_Shift | ImGuiKey_PageUp, R)) return Action{Op::MoveUp, true, false, page};
    if (sc(ImGuiKey_PageDown, R)) return Action{Op::MoveDown, false, false, page};
    if (sc(ImGuiMod_Shift | ImGuiKey_PageDown, R)) return Action{Op::MoveDown, true, false, page};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_UpArrow) || sc(ImGuiMod_Ctrl | ImGuiKey_Home)) return Action{Op::MoveToTop, false};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_UpArrow) || sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Home)) return Action{Op::MoveToTop, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_DownArrow) || sc(ImGuiMod_Ctrl | ImGuiKey_End)) return Action{Op::MoveToBottom, false};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_DownArrow) || sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_End)) return Action{Op::MoveToBottom, true};

    if (sc(ImGuiKey_Home)) return Action{Op::MoveToStartOfLine, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_Home)) return Action{Op::MoveToStartOfLine, true};
    if (sc(ImGuiKey_End)) return Action{Op::MoveToEndOfLine, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_End)) return Action{Op::MoveToEndOfLine, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_A)) return Action{Op::SelectAll};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiKey_D)) return Action{Op::AddNextOccurrence, false, false};
    if (hasSelection && (sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_D) || sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_L))) return Action{Op::AddNextOccurrence, false, true};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D)) return Action{Op::SelectAllOccurrences, false, false};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_D)) return Action{Op::SelectAllOccurrences, false, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_X) || sc(ImGuiMod_Ctrl | ImGuiKey_Delete)) return Action{Op::Cut};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_C) || sc(ImGuiMod_Ctrl | ImGuiKey_Insert)) return Action{Op::Copy};
    if (!ro && (sc(ImGuiMod_Ctrl | ImGuiKey_V, R) || sc(ImGuiMod_Ctrl | ImGuiKey_Insert, R))) return Action{Op::Paste};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_Z, R)) return Action{Op::Undo};
    if (!ro && (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, R) || sc(ImGuiMod_Ctrl | ImGuiKey_Y, R))) return Action{Op::Redo};

    if (!ro && (sc(ImGuiKey_Delete, R) || sc(ImGuiMod_Shift | ImGuiKey_Delete, R))) return Action{Op::Delete, false, false};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_Delete, R)) return Action{Op::Delete, false, true};
    if (!ro && (sc(ImGuiKey_Backspace, R) || sc(ImGuiMod_Shift | ImGuiKey_Backspace, R))) return Action{Op::Backspace, false, false};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_Backspace, R)) return Action{Op::Backspace, false, true};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K)) return Action{Op::RemoveSelectedLines};

    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_LeftBracket, R)) return Action{Op::DeindentLines};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_RightBracket, R)) return Action{Op::IndentLines};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_UpArrow)) return Action{Op::MoveUpLines};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_DownArrow)) return Action{Op::MoveDownLines};

    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiKey_F)) return Action{Op::OpenFindReplace};
    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F)) return Action{Op::FindAll};
    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiKey_G, R)) return Action{Op::Find};

    if (!ro && sc(s.Config.AutoComplete.TriggerShortcut)) return Action{Op::AutoCompleteShortcut};

    if (sc(ImGuiKey_Insert)) return Action{Op::ToggleOverwrite};

    if (!ro && (sc(ImGuiKey_Enter, R) || sc(ImGuiKey_KeypadEnter, R))) return Action{Op::Character, false, false, 0, '\n'};
    if (!ro && (sc(ImGuiMod_Ctrl | ImGuiKey_Enter, R) || sc(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter, R))) return Action{Op::InsertLineBelow};
    if (!ro && (sc(ImGuiMod_Shift | ImGuiKey_Enter, R) || sc(ImGuiMod_Shift | ImGuiKey_KeypadEnter, R))) return Action{Op::InsertLineAbove};

    if (!ro && sc(ImGuiKey_Tab, R)) return Action{Op::Tab};
    if (!ro && sc(ImGuiMod_Shift | ImGuiKey_Tab, R)) return Action{Op::DeindentLines};

    if ((s.AutoComplete.Active || s.FindReplaceVisible || HasMultiple(s.Cursors)) && sc(ImGuiKey_Escape)) {
        if (s.AutoComplete.Active) return Action{Op::AutoCompleteCancel};
        if (s.FindReplaceVisible) return Action{Op::CloseFindReplace};
        return Action{Op::ClearAdditionalCursors};
    }
    return std::nullopt;
}

bool IsSpecialKeyPressed() {
    return std::ranges::any_of(std::array{ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_UpArrow, ImGuiKey_DownArrow}, [](ImGuiKey key) { return ImGui::IsKeyPressed(key); });
}

void HandleKeyboardInputs(State &s, const Metrics &m) {
    if (!ImGui::IsWindowFocused()) return;
    auto &io = ImGui::GetIO();

    // The autocomplete popup handles these keys while it has suggestions.
    if (s.AutoComplete.Active && IsSpecialKeyPressed()) {
        if (!s.AutoComplete.Suggestions.empty()) return;
        Apply(s, Action{Op::AutoCompleteCancel});
    }

    if (const auto action = KeyboardAction(s)) {
        if (action->Kind != Op::Paste) Apply(s, *action);
        else if (const auto *clipboard = ImGui::GetClipboardText()) Apply(s, Action{.Kind = Op::Paste, .Text = clipboard});
    }

    if (!s.Config.ReadOnly && !(ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsKeyDown(ImGuiMod_Alt))) {
        for (const auto character : io.InputQueueCharacters)
            if (character == '\n' || character >= 32) Apply(s, Action{Op::Character, false, false, 0, character});
    }
    io.InputQueueCharacters.resize(0);

    if (!s.Config.ReadOnly) {
        auto &ime = ImGui::GetCurrentContext()->PlatformImeData;
        ime.WantVisible = true;
        ime.WantTextInput = true;
        const auto cursorPos = ScreenPos(m, DocToVis(s, MainCursor(s.Cursors).End));
        ime.InputPos = ImVec2(cursorPos.x - 1.0f, cursorPos.y);
        ime.InputLineHeight = m.GlyphSize.y;
        ime.ViewportId = ImGui::GetCurrentWindow()->Viewport->ID;
    }
    ImGui::SetNextFrameWantCaptureKeyboard(true);
}

void HandleMouseInteractions(State &s, const Metrics &m) {
    const auto &io = ImGui::GetIO();
    const auto mousePos = ImGui::GetMousePos() - m.CursorScreenPos;
    const auto absoluteMousePos = ImGui::GetMousePos() - ImGui::GetWindowPos();
    const auto textMousePos = mousePos - s.Scroll;

    const auto overLineNumbers = s.Config.ShowLineNumbers && absoluteMousePos.x > m.LineNumberLeftOffset && absoluteMousePos.x < m.LineNumberRightOffset;
    const auto overMiniMap = s.Config.ShowMiniMap && absoluteMousePos.x > m.TextRightOffset;
    const auto overText = textMousePos.x > m.TextLeftOffset && textMousePos.x < m.TextRightOffset && textMousePos.y >= 0 && textMousePos.y < m.TextSize.y;

    const auto [glyphPos, cursorPos] = ScreenToDoc(s, ImVec2((mousePos.x - m.TextLeftOffset) / m.GlyphSize.x, mousePos.y / m.GlyphSize.y));

    s.Panning &= s.Config.PanMode && ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    if (s.Panning && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const auto windowSize = ImGui::GetWindowSize();
        auto mouseDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        const float dragFactor = io.DeltaTime * 15.0f;
        const ImVec2 autoPanMargin(m.GlyphSize.x * 4.0f, m.GlyphSize.y * 2.0f);
        if (absoluteMousePos.x < m.TextLeftOffset + autoPanMargin.x) mouseDelta.x = (absoluteMousePos.x - (m.TextLeftOffset + autoPanMargin.x)) * dragFactor;
        else if (absoluteMousePos.x > windowSize.x - autoPanMargin.x) mouseDelta.x = (absoluteMousePos.x - (windowSize.x - autoPanMargin.x)) * dragFactor;
        if (absoluteMousePos.y < autoPanMargin.y) mouseDelta.y = (absoluteMousePos.y - autoPanMargin.y) * dragFactor;
        else if (absoluteMousePos.y > windowSize.y - autoPanMargin.y) mouseDelta.y = (absoluteMousePos.y - (windowSize.y - autoPanMargin.y)) * dragFactor;
        Apply(s, Action{.Kind = Op::PanBy, .Vec = mouseDelta});
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    } else if (s.Scrolling) {
        // Shrinks toward zero by a glyph width.
        const auto deadzone = [&](float v) { return v < 0.0f ? std::min(v + m.GlyphSize.x, 0.0f) : std::max(v - m.GlyphSize.x, 0.0f); };
        const auto offset = s.ScrollStart - absoluteMousePos;
        Apply(s, Action{.Kind = Op::PanBy, .Vec = ImVec2(deadzone(offset.x), deadzone(offset.y)) * (io.DeltaTime * 5.0f)});
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) Apply(s, Action{Op::StopScrollMode});
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        if (s.MiniMapIsScrollbar) Apply(s, Action{.Kind = Op::MiniMapDrag, .Vec = ImVec2(m.TextSize.y, absoluteMousePos.y)});
        else if (s.SelectingText && (overLineNumbers || overText)) Apply(s, Action{.Kind = overLineNumbers ? Op::DragLineSelect : Op::DragSelect, .Pos = cursorPos});
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        Apply(s, Action{Op::MouseReleased});
    } else if (ImGui::IsWindowHovered()) {
        if (ImGui::IsWindowFocused() && overText) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            Apply(s, s.Config.PanMode ? Action{Op::BeginPan} : Action{.Kind = Op::BeginScrollMode, .Vec = absoluteMousePos});
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Upstream opens context menus here through host callbacks, which are out of scope.
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            s.SelectingText = overText || overLineNumbers;
            const auto doubleClick = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
            const auto now = float(ImGui::GetTime());
            const auto tripleClick = !doubleClick && s.LastClickTime != -1.0f && (now - s.LastClickTime) < io.MouseDoubleClickTime;
            s.LastClickTime = tripleClick ? -1.0f : now;

            if (tripleClick) {
                if (overText) Apply(s, Action{.Kind = Op::TripleClick, .Pos = cursorPos});
            } else if (doubleClick) {
                if (overText) Apply(s, Action{.Kind = Op::DoubleClick, .Select = ImGui::IsKeyDown(ImGuiMod_Shift), .Pos = glyphPos});
            } else if (overLineNumbers || overText) {
                const auto addCursor = ImGui::IsKeyDown(io.ConfigMacOSXBehaviors ? ImGuiMod_Alt : ImGuiMod_Ctrl);
                Apply(s, Action{.Kind = overLineNumbers ? Op::ClickLineNumbers : Op::ClickText, .Select = ImGui::IsKeyDown(ImGuiMod_Shift), .Word = addCursor, .Pos = cursorPos});
            } else if (overMiniMap && ImGui::GetCurrentWindow()->ScrollbarY) {
                const auto clickedRow = s.FirstMiniMapRow + uint64_t(absoluteMousePos.y / m.MiniMapRowHeight);
                const auto inView = clickedRow >= s.FirstVisibleRow && clickedRow <= s.LastVisibleRow;
                Apply(s, Action{.Kind = Op::ClickMiniMap, .Select = inView, .Count = VisToDoc(s, {clickedRow, 0}).Line, .Vec = ImVec2(0, absoluteMousePos.y)});
            }
        }

        if (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right)) Apply(s, Action{Op::MouseDown});
    }
}

void RenderCurrentLineHighlight(const State &s, const Metrics &m) {
    if (!s.Config.ShowCurrentLineHighlight || AnyHasSelection(s.Cursors)) return;
    std::vector<uint64_t> drawn;
    for (const auto &c : s.Cursors.Items) {
        const auto line = c.End.Line;
        if (std::ranges::contains(drawn, line)) continue;
        drawn.push_back(line);
        const auto &layout = LineAt(s, line);
        // Upstream omits the window x position here. See UpstreamBugs.md.
        const auto topLeft = ImVec2(m.TextLeftOffset, m.CursorScreenPos.y + layout.Row * m.GlyphSize.y);
        const auto bottomRight = topLeft + ImVec2(m.TextRightOffset, layout.Rows * m.GlyphSize.y);
        m.Draw->AddRectFilled(topLeft, bottomRight, Shade(s, Color::CurrentLineHighlight));
        m.Draw->AddRect(topLeft, bottomRight, Shade(s, Color::CurrentLineHighlightBorder));
    }
}

void RenderActiveBracketBackground(const State &s, const Metrics &m) {
    if (!s.Config.ShowMatchingBrackets || s.Derived.Brackets.empty()) return;
    const auto *active = EnclosingBrackets(s.Derived, MainCursor(s.Cursors).End);
    if (!active || !active->Visible) return;
    for (const auto pos : {active->Start, active->End}) {
        if (IsHidden(s, pos.Line)) continue;
        const auto vis = DocToVis(s, pos);
        if (vis.Row < s.FirstVisibleRow || vis.Row > s.LastVisibleRow) continue;
        const auto topLeft = ScreenPos(m, vis);
        m.Draw->AddRectFilled(topLeft, topLeft + m.GlyphSize, Shade(s, Color::MatchingBracketBackground));
    }
}

void RenderMatchingBracketLines(const State &s, const Metrics &m) {
    if (!s.Config.ShowMatchingBrackets || s.Derived.Brackets.empty()) return;
    const auto *active = EnclosingBrackets(s.Derived, MainCursor(s.Cursors).End);
    for (const auto &bracket : s.Derived.Brackets) {
        if (!bracket.Visible || bracket.End.Line - bracket.Start.Line <= 1) continue;
        const auto column = std::min(DocToVis(s, bracket.Start).Column, DocToVis(s, bracket.End).Column);
        const auto color = Shade(s, &bracket == active ? Color::MatchingBracketActive : Color::Whitespace);
        for (auto i = bracket.Start.Line + 1; i < bracket.End.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto &line = LineAt(s, i);
            const auto top = ScreenPos(m, {line.Row, column});
            m.Draw->AddLine(top, ImVec2(top.x, top.y + line.Rows * m.GlyphSize.y), color);
        }
    }
}

void RenderSelections(const State &s, const Metrics &m) {
    const auto fill = [&](uint64_t row, uint64_t left, uint64_t right) {
        const auto topLeft = ScreenPos(m, {row, left});
        m.Draw->AddRectFilled(topLeft, ImVec2(ScreenPos(m, {row, right}).x, topLeft.y + m.GlyphSize.y), Shade(s, Color::Selection));
    };
    for (const auto &c : s.Cursors.Items) {
        if (!c.HasSelection()) continue;
        const auto begin = c.SelectionStart(), end = c.SelectionEnd();
        for (auto i = begin.Line; i <= end.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto &line = LineAt(s, i);
            if (line.Rows == 1) {
                if (line.Row < s.FirstVisibleRow || line.Row > s.LastVisibleRow) continue;
                const DocPos lineLeft{i, 0}, lineRight{i, LineLength(s.Doc, i)};
                fill(line.Row, DocToVis(s, std::max(begin, lineLeft)).Column, DocToVis(s, std::min(end, lineRight)).Column);
            } else {
                for (uint64_t j = 0; j < line.Rows; ++j) {
                    const auto &section = s.Layout.Sections[line.FirstSection + j];
                    const auto row = line.Row + j;
                    if (row < s.FirstVisibleRow || row > s.LastVisibleRow) continue;
                    const DocPos sectionLeft{i, section.StartIndex}, sectionRight{i, section.EndIndex};
                    if (!(begin < sectionRight && end > sectionLeft)) continue;
                    fill(row, begin <= sectionLeft ? 0 : DocToVis(s, begin).Column, end > sectionRight ? section.Columns : DocToVis(s, end).Column);
                }
            }
        }
    }
}

// Fills each visible marker row from x `left` to `right` with the marker's `color`, and shows its `tooltip` on hover.
void RenderMarkers(const State &s, const Metrics &m, float left, float right, ImU32 Marker::*color, std::string Marker::*tooltip) {
    ForEachMarker(s, s.FirstVisibleRow, s.LastVisibleRow + 1, [&](uint64_t row, const Marker &marker) {
        if (!(marker.*color & IM_COL32_A_MASK)) return;
        const auto y = m.CursorScreenPos.y + row * m.GlyphSize.y;
        const ImVec2 start(left, y), end(right, y + m.GlyphSize.y);
        m.Draw->AddRectFilled(start, end, marker.*color);
        if (!(marker.*tooltip).empty() && ImGui::IsMouseHoveringRect(start, end) && ImGui::BeginTooltip()) {
            ImGui::PushStyleColor(ImGuiCol_PopupBg, marker.*color);
            ImGui::TextUnformatted((marker.*tooltip).c_str());
            ImGui::PopStyleColor();
            ImGui::EndTooltip();
        }
    });
}

// Draws `squiggle` from x `left` to the glyph at `end`, and its tooltip on hover.
void RenderSquiggle(const Squiggle &squiggle, const Metrics &m, float left, ImVec2 end) {
    const ImVec2 topLeft{left, end.y}, bottomRight{end.x, end.y + m.GlyphSize.y};
    switch (squiggle.Style) {
        case SquiggleStyle::Wave: {
            const auto height = bottomRight.y - topLeft.y;
            const auto size = height * 0.2f;
            const auto offset = topLeft.y + height * 0.8f;
            ImVec2 point(left, offset);
            bool down = true;
            m.Draw->PushClipRect(topLeft, bottomRight, true);
            while (point.x < bottomRight.x) {
                const ImVec2 next{point.x + size, down ? offset + size : offset};
                m.Draw->AddLine(point, next, squiggle.Shade, 1.2f * m.FontScaleDpi);
                point = next;
                down = !down;
            }
            m.Draw->PopClipRect();
            break;
        }
        case SquiggleStyle::Background: m.Draw->AddRectFilled(topLeft, bottomRight, squiggle.Shade); break;
    }
    if (!squiggle.Tooltip.empty() && ImGui::IsMouseHoveringRect(topLeft, bottomRight) && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(squiggle.Tooltip.c_str());
        ImGui::EndTooltip();
    }
}

void RenderSquiggles(const State &s, const Metrics &m) {
    if (s.Squiggles.empty()) return;
    ImVec2 rowScreenPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset, s.FirstVisibleRow * m.GlyphSize.y);
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        uint64_t id = 0; // Of the squiggle being drawn, zero for none.
        float left = 0.0f;
        // Draws the current squiggle up to `end` and starts squiggle `next` there.
        const auto flush = [&](ImVec2 end, uint64_t next) {
            if (id) RenderSquiggle(s.Squiggles[id - 1], m, left, end);
            id = next;
            left = end.x;
        };
        for (RowWalk walk(s, row.Line, row.Section); walk.Column < row.Columns && walk.Column <= s.LastVisibleColumn; walk.Step()) {
            const auto next = SquiggleAt(s, walk.Start + walk.Index);
            if (next != id) flush(ImVec2(rowScreenPos.x + walk.Column * m.GlyphSize.x, rowScreenPos.y), next);
        }
        flush(m.CursorScreenPos + ImVec2(m.TextLeftOffset + row.Columns * m.GlyphSize.x, i * m.GlyphSize.y), 0);
        rowScreenPos.y += m.GlyphSize.y;
    }
}

ImU32 MarkerMiniColor(const Marker &marker) { return marker.TextColor ? marker.TextColor : marker.LineNumberColor; }

void RenderText(const State &s, const Metrics &m) {
    ImVec2 rowScreenPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset, s.FirstVisibleRow * m.GlyphSize.y);
    const auto firstRenderableColumn = (s.FirstVisibleColumn / s.Config.TabSize) * s.Config.TabSize;
    const auto whitespace = Shade(s, Color::Whitespace);
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        for (RowWalk walk(s, row.Line, row.Section); walk.Column < row.Columns && walk.Column <= s.LastVisibleColumn; walk.Step()) {
            if (walk.Column < firstRenderableColumn) continue;
            const auto offset = walk.Start + walk.Index, cp = s.Doc[offset];
            const ImVec2 glyphPos(rowScreenPos.x + walk.Column * m.GlyphSize.x, rowScreenPos.y);
            if (cp == '\t') {
                if (s.Config.ShowTabs) {
                    const auto x1 = glyphPos.x + m.GlyphSize.x * 0.3f;
                    const auto y = glyphPos.y + m.FontSize * 0.5f;
                    const auto x2 = glyphPos.x + m.GlyphSize.x;
                    const ImVec2 p1(x1, y), p2(x2, y), p3(x2 - m.FontSize * 0.16f, y - m.FontSize * 0.16f), p4(x2 - m.FontSize * 0.16f, y + m.FontSize * 0.16f);
                    m.Draw->AddLine(p1, p2, whitespace);
                    m.Draw->AddLine(p2, p3, whitespace);
                    m.Draw->AddLine(p2, p4, whitespace);
                }
            } else if (cp == ' ') {
                if (s.Config.ShowSpaces) m.Draw->AddCircleFilled(ImVec2(glyphPos.x + m.GlyphSize.x * 0.5f, glyphPos.y + m.FontSize * 0.5f), 1.5f, whitespace, 4);
            } else {
                m.Font->RenderChar(m.Draw, m.FontSize, glyphPos, Shade(s, ColorAt(s, offset)), ImWchar(cp));
            }
        }
        const auto &line = LineAt(s, row.Line);
        if (i == line.Row + line.Rows - 1 && FoldAt(s, row.Line) == FoldingState::Folded) {
            const auto glyphPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset + row.Columns * m.GlyphSize.x, i * m.GlyphSize.y);
            m.Font->RenderChar(m.Draw, m.FontSize, glyphPos, Shade(s, Color::Text), m.Font->EllipsisChar);
        }
        rowScreenPos.y += m.GlyphSize.y;
    }
}

void RenderCursorCarets(State &s, const Metrics &m) {
    if (!s.Config.CaretsVisible || !ImGui::IsWindowFocused()) return;
    const auto &io = ImGui::GetIO();
    s.CaretTimer += io.DeltaTime;
    if (io.ConfigInputTextCursorBlink && s.CaretTimer > 0.0f && std::fmod(s.CaretTimer, 1.2f) > 0.8f) return;
    for (const auto &c : s.Cursors.Items) {
        if (IsHidden(s, c.End.Line)) continue;
        const auto pos = DocToVis(s, c.End);
        if (pos.Row < s.FirstVisibleRow || pos.Row > s.LastVisibleRow || pos.Column < s.FirstVisibleColumn || pos.Column > s.LastVisibleColumn) continue;
        const auto p = ScreenPos(m, pos);
        m.Draw->AddRectFilled(ImVec2(p.x - s.CursorWidth, p.y), ImVec2(p.x, p.y + m.GlyphSize.y), Shade(s, Color::Cursor));
    }
}

void RenderMiniMap(State &s, const Metrics &m) {
    if (!s.Config.ShowMiniMap) return;
    std::vector<ImU32> rowColors(s.MiniRows.size(), 0);
    ForEachSelectedLine(s, [&](const LineLayout &line) { std::fill_n(rowColors.begin() + int64_t(line.Row), line.Rows, s.MiniShaded[size_t(Color::Selection)]); });
    ForEachMarker(s, 0, RowCount(s), [&](uint64_t row, const Marker &marker) { rowColors[row] = ScaleAlpha(MarkerMiniColor(marker), MiniMapAlpha); });
    const auto totalMiniMapRows = s.MiniRows.size();
    const auto visibleMiniMapRows = m.TextSize.y / m.MiniMapRowHeight;
    if (totalMiniMapRows < visibleMiniMapRows) {
        s.FirstMiniMapRow = 0;
        s.LastMiniMapRow = totalMiniMapRows;
    } else {
        const auto scrollRatio = ImGui::GetScrollY() / ImGui::GetScrollMaxY();
        s.FirstMiniMapRow = uint64_t(scrollRatio * (totalMiniMapRows - visibleMiniMapRows));
        s.LastMiniMapRow = std::min(s.FirstMiniMapRow + uint64_t(std::ceil(visibleMiniMapRows)), uint64_t(totalMiniMapRows));
    }
    auto pos = ImGui::GetWindowPos() + ImVec2(m.TextRightOffset, 0.0f);
    for (auto i = s.FirstMiniMapRow; i < s.LastMiniMapRow; ++i) {
        if (rowColors[i]) m.Draw->AddRectFilled(pos, pos + ImVec2(m.MiniMapWidth, m.MiniMapRowHeight), rowColors[i]);
        for (const auto &section : s.MiniRows[i].Sections)
            m.Draw->AddRectFilled(pos + ImVec2(section.Start * m.MiniMapColumnWidth, 0.0f), pos + ImVec2(section.End * m.MiniMapColumnWidth, m.MiniMapColumnHeight), s.MiniShaded[size_t(section.Shade)]);
        pos += ImVec2(0.0f, m.MiniMapRowHeight);
    }
    if (s.TotalSize.y > m.TextSize.y) {
        const auto viewPortStart = (s.FirstVisibleRow - s.FirstMiniMapRow) * m.MiniMapRowHeight;
        const auto viewportHeight = (s.LastVisibleRow - s.FirstVisibleRow) * m.MiniMapRowHeight;
        const auto topLeft = ImGui::GetWindowPos() + ImVec2(m.TextRightOffset, viewPortStart);
        const auto bottomRight = topLeft + ImVec2(m.MiniMapWidth, viewportHeight);
        m.Draw->AddRectFilled(topLeft, bottomRight, ScaleAlpha(Shade(s, Color::Text), s.MiniMapIsScrollbar ? MiniMapViewPortActiveAlpha : MiniMapViewPortAlpha));
    }
}

void RenderPanScrollIndicator(const State &s, const Metrics &m) {
    if (!s.Config.ShowPanScrollIndicator || !(s.Panning || s.Scrolling)) return;
    const auto center = ImGui::GetWindowPos() + ImGui::GetWindowSize() / 2.0f;
    constexpr int alpha = 160;
    m.Draw->AddCircleFilled(center, 20.0f, IM_COL32(255, 255, 255, alpha));
    m.Draw->AddCircle(center, 5.0f, IM_COL32(0, 0, 0, alpha), 0, 2.0f);
    m.Draw->AddTriangle(ImVec2(center.x - 15.0f, center.y), ImVec2(center.x - 8.0f, center.y - 4.0f), ImVec2(center.x - 8.0f, center.y + 4.0f), IM_COL32(0, 0, 0, alpha), 2.0f);
    m.Draw->AddTriangle(ImVec2(center.x + 15.0f, center.y), ImVec2(center.x + 8.0f, center.y - 4.0f), ImVec2(center.x + 8.0f, center.y + 4.0f), IM_COL32(0, 0, 0, alpha), 2.0f);
    m.Draw->AddTriangle(ImVec2(center.x, center.y - 15.0f), ImVec2(center.x - 4.0f, center.y - 8.0f), ImVec2(center.x + 4.0f, center.y - 8.0f), IM_COL32(0, 0, 0, alpha), 2.0f);
    m.Draw->AddTriangle(ImVec2(center.x, center.y + 15.0f), ImVec2(center.x - 4.0f, center.y + 8.0f), ImVec2(center.x + 4.0f, center.y + 8.0f), IM_COL32(0, 0, 0, alpha), 2.0f);
}

void RenderFoldIndicators(State &s, const Metrics &m) {
    if (!s.Config.LineFolding) return;
    const auto size = m.FontSize * 0.25f;
    const ImVec2 half(m.GlyphSize.x / 2.0f, m.FontSize / 2.0f);
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        const auto foldingState = FoldAt(s, row.Line);
        if (row.Section != 0 || (foldingState != FoldingState::Foldable && foldingState != FoldingState::Folded)) continue;
        const auto center = ImVec2(ImGui::GetWindowPos().x + m.FoldIndicatorOffset, m.CursorScreenPos.y + m.GlyphSize.y * i + m.FontSize / 2.0f);
        const auto isHovered = ImGui::IsMouseHoveringRect(center - half, center + half);
        const auto color = Shade(s, isHovered ? Color::CurrentLineNumber : Color::LineNumber);
        if (foldingState == FoldingState::Foldable) {
            m.Draw->AddLine(ImVec2(center.x - size, center.y - size * 0.5f), ImVec2(center.x, center.y + size), color);
            m.Draw->AddLine(ImVec2(center.x, center.y + size), ImVec2(center.x + size, center.y - size * 0.5f), color);
        } else {
            m.Draw->AddLine(ImVec2(center.x - size * 0.5f, center.y - size), ImVec2(center.x + size, center.y), color);
            m.Draw->AddLine(ImVec2(center.x + size, center.y), ImVec2(center.x - size * 0.5f, center.y + size), color);
        }
        if (isHovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) Apply(s, Action{.Kind = Op::ToggleFold, .Pos = {row.Line, 0}});
    }
    ImGui::SetCursorScreenPos(m.CursorScreenPos);
}

void RenderScrollbarMiniMap(const State &s, const Metrics &m) {
    if (!s.Config.ShowScrollbarMiniMap) return;
    auto *window = ImGui::GetCurrentWindow();
    if (!window->ScrollbarY) return;
    const auto rect = ImGui::GetWindowScrollbarRect(window, ImGuiAxis_Y);
    const auto rowHeight = rect.GetHeight() / float(RowCount(s));
    const auto offset = rect.GetWidth() * 0.3f;
    const auto left = rect.Min.x + offset, right = rect.Max.x - offset;
    m.Draw->PushClipRect(rect.Min, rect.Max, false);
    ForEachSelectedLine(s, [&](const LineLayout &line) {
        const auto ly1 = std::round(rect.Min.y + line.Row * rowHeight), ly2 = std::round(rect.Min.y + (line.Row + line.Rows) * rowHeight);
        m.Draw->AddRectFilled(ImVec2(left, ly1), ImVec2(right, ly2), Shade(s, Color::Selection));
    });
    ForEachMarker(s, 0, RowCount(s), [&](uint64_t row, const Marker &marker) {
        const auto ly = std::round(rect.Min.y + row * rowHeight);
        m.Draw->AddRectFilled(ImVec2(left, ly), ImVec2(right, ly + rowHeight), MarkerMiniColor(marker));
    });
    m.Draw->PopClipRect();
}

bool LatchButton(const char *label, bool value, ImVec2 size) {
    const auto *colors = ImGui::GetStyle().Colors;
    const auto on = colors[ImGuiCol_ButtonActive], off = colors[ImGuiCol_TableBorderLight];
    ImGui::PushStyleColor(ImGuiCol_Button, value ? on : off);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, value ? on : off);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, value ? off : on);
    ImGui::Button(label, size);
    const auto clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    ImGui::PopStyleColor(3);
    return clicked;
}

bool InputString(const char *label, std::string &value, ImGuiInputTextFlags flags = 0) {
    flags |= ImGuiInputTextFlags_NoUndoRedo | ImGuiInputTextFlags_CallbackResize;
    return ImGui::InputText(label, value.data(), value.capacity() + 1, flags, [](ImGuiInputTextCallbackData *data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto *value = static_cast<std::string *>(data->UserData);
            value->resize(data->BufTextLen);
            data->Buf = value->data();
        }
        return 0; }, &value);
}

void RenderFindReplace(State &s, const Metrics &m) {
    if (!s.FindReplaceVisible) return;
    const auto currentScreenPosition = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    const auto &style = ImGui::GetStyle();
    const auto fieldWidth = 250.0f * style.FontScaleDpi;
    const auto buttonWidth = [&](const char *label) { return ImGui::CalcTextSize(label).x + style.ItemSpacing.x * 2.0f; };
    auto button1Width = buttonWidth(s.Config.FindButtonLabel.c_str());
    auto button2Width = buttonWidth(s.Config.FindAllButtonLabel.c_str());
    const auto optionWidth = buttonWidth("Aa");
    if (!s.Config.ReadOnly) {
        button1Width = std::max(button1Width, buttonWidth(s.Config.ReplaceButtonLabel.c_str()));
        button2Width = std::max(button2Width, buttonWidth(s.Config.ReplaceAllButtonLabel.c_str()));
    }
    const auto windowHeight = style.ChildBorderSize * 2.0f + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight() + (s.Config.ReadOnly ? 0.0f : (style.ItemSpacing.y + ImGui::GetFrameHeight()));
    const auto windowWidth = style.ChildBorderSize * 2.0f + style.WindowPadding.x * 2.0f + fieldWidth + style.ItemSpacing.x + button1Width + style.ItemSpacing.x + button2Width + style.ItemSpacing.x + optionWidth * 3.0f + style.ItemSpacing.x * 2.0f;
    const auto availableSpace = ImGui::GetWindowWidth() - m.MiniMapWidth - (ImGui::GetCurrentWindow()->ScrollbarY ? style.ScrollbarSize : 0.0f);
    ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImVec2(availableSpace - windowWidth - style.ItemSpacing.x, style.ItemSpacing.y * 2.0f));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    ImGui::SetNextWindowBgAlpha(0.75f);
    // Draws the two buttons after a field, disabled if `disabled`.
    const auto buttons = [&](bool disabled, const std::string &label1, Op op1, const std::string &label2, Op op2) {
        ImGui::SameLine();
        ImGui::BeginDisabled(disabled);
        if (ImGui::Button(label1.c_str(), ImVec2(button1Width, 0.0f))) Apply(s, Action{op1});
        ImGui::SameLine();
        if (ImGui::Button(label2.c_str(), ImVec2(button2Width, 0.0f))) Apply(s, Action{op2});
        ImGui::EndDisabled();
    };

    ImGui::BeginChild("find-replace", ImVec2(windowWidth, windowHeight), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(fieldWidth);
    if (std::exchange(s.FocusOnFind, false)) ImGui::SetKeyboardFocusHere();
    else if (std::exchange(s.FindCancelledAutocomplete, false)) ImGui::SetKeyboardFocusHere();
    // The field edits a copy. A change is applied as an action.
    auto findText = s.FindText;
    if (InputString("###find", findText, ImGuiInputTextFlags_AutoSelectAll)) Apply(s, Action{.Kind = Op::SelectFirstOccurrence, .Text = findText});
    if (ImGui::IsItemDeactivated()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) Apply(s, Action{Op::CloseFindReplace});
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) Apply(s, Action{Op::FocusEditor});
    }

    const bool disableFindButtons = s.FindText.empty();
    buttons(disableFindButtons, s.Config.FindButtonLabel, Op::Find, s.Config.FindAllButtonLabel, Op::FindAll);
    ImGui::SameLine();
    if (LatchButton("Aa", s.CaseSensitiveFind, ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::ToggleCaseSensitiveFind});
    ImGui::SameLine();
    if (LatchButton("[]", s.WholeWordFind, ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::ToggleWholeWordFind});
    ImGui::SameLine();
    if (ImGui::Button("x", ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::CloseFindReplace});

    if (!s.Config.ReadOnly) {
        ImGui::SetNextItemWidth(fieldWidth);
        InputString("###replace", s.ReplaceText);
        buttons(s.FindText.empty() || s.ReplaceText.empty(), s.Config.ReplaceButtonLabel, Op::Replace, s.Config.ReplaceAllButtonLabel, Op::ReplaceAll);
    }

    if (ImGui::IsWindowFocused() && !disableFindButtons && ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_G)) {
        ImGui::SetWindowFocus(nullptr);
        Apply(s, Action{Op::Find});
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(currentScreenPosition);
}

bool RenderSuggestion(std::string_view suggestion, std::string_view searchTerm, float width, bool selected) {
    auto glyphPos = ImGui::GetCursorScreenPos();
    const auto size = ImVec2(width, ImGui::GetFrameHeightWithSpacing());
    const auto clicked = ImGui::InvisibleButton("suggestion", size);
    auto *drawList = ImGui::GetWindowDrawList();
    auto *font = ImGui::GetFont();
    const auto fontSize = ImGui::GetFontSize();
    const auto glyphWidth = ImGui::CalcTextSize("#").x;
    if (selected) drawList->AddRectFilled(glyphPos, glyphPos + size, ImGui::GetColorU32(ImGuiCol_Header));
    glyphPos += ImGui::GetStyle().FramePadding;
    const auto term = Codepoints(searchTerm);
    size_t j = 0;
    for (const auto cp : Codepoints(suggestion)) {
        const auto match = j < term.size() && ToLower(term[j]) == ToLower(cp);
        if (match) ++j;
        font->RenderChar(drawList, fontSize, glyphPos, ImGui::GetColorU32(match ? ImGuiCol_TextLink : ImGuiCol_Text), ImWchar(cp));
        glyphPos.x += glyphWidth;
    }
    return clicked;
}

void RenderAutoComplete(State &s, const Metrics &m) {
    auto &ac = s.AutoComplete;
    const auto &config = s.Config.AutoComplete;
    if (ac.RequestActivation && s.ClockUs > ac.ActivationTimeUs) {
        ac.RequestActivation = false;
        ac.StartLocation = FindWordStart(s.Doc, ac.CurrentLocation, true);
        UpdateAutoCompleteContext(s);
        if (ac.Context.InComment && !config.TriggerInComments) return;
        if (ac.Context.InString && !config.TriggerInStrings) return;
        RefreshSuggestions(s);
        ImGui::OpenPopup("AutoCompleteContextMenu");
        ac.Active = true;
    }
    if (!ac.Active) return;

    const auto newLocation = MainCursor(s.Cursors).SelectionEnd();
    if (newLocation != ac.CurrentLocation) {
        if (newLocation.Line != ac.CurrentLocation.Line || FindWordStart(s.Doc, newLocation, true) != ac.StartLocation) {
            ac.RequestDeactivation = true;
        } else {
            ac.CurrentLocation = newLocation;
            if (newLocation == ac.StartLocation) {
                ac.RequestDeactivation = true;
            } else {
                UpdateAutoCompleteContext(s);
                RefreshSuggestions(s);
            }
        }
    }

    bool result = false;
    const auto pos = DocToVis(s, ac.CurrentLocation);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetCursorScreenPos().x + m.TextLeftOffset + pos.Column * m.GlyphSize.x, ImGui::GetCursorScreenPos().y + (pos.Row + 1) * m.GlyphSize.y));
    const auto suggestions = ac.Suggestions.size();
    if (suggestions == 0 && !ac.TriggeredManually) ac.RequestDeactivation = true;
    const auto visibleSuggestions = std::clamp(suggestions, size_t(1), size_t(10));
    const auto height = ImGui::GetFrameHeightWithSpacing() * visibleSuggestions + ImGui::GetStyle().WindowPadding.y * 2.0f;
    ImGui::SetNextWindowSize(ImVec2(config.SuggestionWidth * m.GlyphSize.x, height));
    if (ImGui::BeginPopup("AutoCompleteContextMenu", ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        if (ImGui::IsWindowAppearing()) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (ac.RequestDeactivation) {
            ImGui::CloseCurrentPopup();
            ac.RequestDeactivation = false;
            ac.Active = false;
        } else if (suggestions) {
            auto scroll = false;
            const bool autoInsert = config.AutoInsertSingleSuggestions && ac.TriggeredManually && suggestions == 1;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                scroll = true;
                ac.CurrentSelection = ac.CurrentSelection == 0 ? suggestions - 1 : ac.CurrentSelection - 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                scroll = true;
                ac.CurrentSelection = ac.CurrentSelection == suggestions - 1 ? 0 : ac.CurrentSelection + 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter) || autoInsert) {
                ac.RequestDeactivation = true;
                result = true;
            }
            for (size_t i = 0; i < suggestions; ++i) {
                ImGui::PushID(int(i));
                const auto selected = i == ac.CurrentSelection;
                if (scroll && selected) ImGui::SetScrollHereY(0.5f);
                if (RenderSuggestion(ac.Suggestions[i], ac.Context.SearchTerm, ImGui::GetContentRegionAvail().x, selected)) {
                    ac.CurrentSelection = i;
                    ac.RequestDeactivation = true;
                    result = true;
                }
                ImGui::PopID();
            }
        } else {
            ImGui::TextUnformatted(config.NoSuggestionsLabel.c_str());
        }
        ImGui::EndPopup();
    } else {
        ac.RequestDeactivation = false;
        ac.Active = false;
    }
    if (result) {
        const auto replacement = ac.CurrentSelection < ac.Suggestions.size() ? ac.Suggestions[ac.CurrentSelection] : std::string();
        Apply(s, Action{.Kind = Op::AutoCompleteAccept, .Text = replacement});
    }
}

void RenderLineNumbers(const State &s, const Metrics &m) {
    if (!s.Config.ShowLineNumbers) return;
    const auto cursorLine = CurrentCursor(s.Cursors).End.Line;
    const auto position = ImVec2(ImGui::GetWindowPos().x + m.LineNumberRightOffset, m.CursorScreenPos.y);
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        if (row.Section != 0) continue;
        const auto lineNo = row.Line + 1;
        const auto width = size_t(std::log10(lineNo) + 1.0f) * m.GlyphSize.x;
        const auto foreground = row.Line == cursorLine ? Color::CurrentLineNumber : Color::LineNumber;
        const auto number = std::to_string(lineNo);
        m.Draw->AddText(position + ImVec2(-width, i * m.GlyphSize.y), Shade(s, foreground), number.c_str());
    }
}
} // namespace

State Initial(std::string_view text, Settings config) {
    State s;
    s.Doc = InsertText(config, {}, {}, text).Doc;
    s.Config = std::move(config);
    ClearAll(s.Cursors);
    s.DocUpdated = true;
    s.ResetScroll = true;
    s.LineFoldsFor = s.Config.LineFolding;
    return s;
}

State Frame(State s, const char *title, ImVec2 size, ImGuiChildFlags childFlags, ImGuiWindowFlags windowFlags) {
    s.ClockUs += int64_t(double(ImGui::GetIO().DeltaTime) * 1e6);
    if (s.FirstFrame) s.FirstFrame = false;
    else if (std::exchange(s.FocusOnEditor, false)) ImGui::SetNextWindowFocus();

    ImGui::SetNextWindowContentSize(s.TotalSize);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Shade(s, Color::Background)));

    if (ImGui::BeginChild(title, size, childFlags, windowFlags)) {
        const ImVec2 current(ImGui::GetScrollX(), ImGui::GetScrollY());
        if (std::exchange(s.ResetScroll, false)) {
            ImGui::SetScrollX(0.0f);
            ImGui::SetScrollY(0.0f);
        } else if (ImGui::IsWindowAppearing() ? s.Scroll != ImVec2() : current == s.ScrollSeen && s.Scroll != current) {
            // The state is from a snapshot: the window has not scrolled since the last frame, but the state says elsewhere.
            ImGui::SetScrollX(s.Scroll.x);
            ImGui::SetScrollY(s.Scroll.y);
        }
        // Read the scroll the child window applied at Begin.
        s.Scroll = s.ScrollSeen = current;

        Metrics m;
        m.Draw = ImGui::GetWindowDrawList();
        m.Font = ImGui::GetFont();
        m.FontSize = ImGui::GetFontSize();
        const auto &style = ImGui::GetStyle();
        m.FontScaleDpi = style.FontScaleDpi;
        m.GlyphSize = ImVec2(ImGui::CalcTextSize("#").x, ImGui::GetTextLineHeightWithSpacing() * s.Config.LineSpacing);
        m.CursorScreenPos = ImGui::GetCursorScreenPos();
        const auto visibleSize = ImGui::GetCurrentWindow()->InnerRect.GetSize();

        const auto digits = s.Config.ShowLineNumbers ? size_t(std::log10(float(LineCount(s.Doc) + 1)) + 1.0f) : 0;
        m.LineNumberLeftOffset = s.Config.ShowLineNumbers ? s.Config.LeftMargin * m.GlyphSize.x : 0.0f;
        m.LineNumberRightOffset = m.LineNumberLeftOffset + digits * m.GlyphSize.x;
        if (s.Config.LineFolding) {
            m.FoldIndicatorOffset = m.LineNumberLeftOffset;
            m.LineNumberLeftOffset += m.GlyphSize.x;
            m.LineNumberRightOffset += m.GlyphSize.x;
        }
        m.TextLeftOffset = m.LineNumberRightOffset + s.Config.TextMargin * m.GlyphSize.x;
        m.MiniMapWidth = 0.0f;
        if (s.Config.ShowMiniMap) {
            m.MiniMapRowHeight = 3.0f * m.FontScaleDpi;
            m.MiniMapColumnHeight = 2.0f * m.FontScaleDpi;
            m.MiniMapColumnWidth = 1.0f * m.FontScaleDpi;
            if (s.Config.MiniMapColumns == 0) m.MiniMapWidth = std::floor((visibleSize.x - m.TextLeftOffset) / (m.GlyphSize.x + m.MiniMapColumnWidth)) * m.MiniMapColumnWidth;
            else m.MiniMapWidth = s.Config.MiniMapColumns * m.MiniMapColumnWidth;
        }
        m.TextRightOffset = visibleSize.x - m.MiniMapWidth;
        m.TextSize = ImVec2(m.TextRightOffset - m.TextLeftOffset, visibleSize.y);
        s.WordWrapColumns = uint64_t(std::max(std::floor(m.TextSize.x / m.GlyphSize.x), 0.0f));

        UpdateState(s, m);
        HandlePossibleScrolling(s, m);
        HandleKeyboardInputs(s, m);
        UpdateState(s, m);
        HandleMouseInteractions(s, m);

        s.FirstVisibleRow = uint64_t(std::floor(s.Scroll.y / m.GlyphSize.y));
        s.LastVisibleRow = std::min(uint64_t(std::ceil((s.Scroll.y + m.TextSize.y) / m.GlyphSize.y)), RowCount(s) - 1);
        s.FirstVisibleColumn = uint64_t(std::floor(s.Scroll.x / m.GlyphSize.x));
        s.LastVisibleColumn = uint64_t(std::ceil((s.Scroll.x + m.TextSize.x) / m.GlyphSize.x));

        if (s.ShadedAlpha != style.Alpha) {
            for (size_t i = 0; i < size_t(Color::Count); ++i) {
                s.Shaded[i] = ImGui::GetColorU32(s.Config.Colors[i]);
                s.MiniShaded[i] = ImGui::GetColorU32(s.Config.Colors[i], MiniMapAlpha);
            }
            s.ShadedAlpha = style.Alpha;
        }
        s.CursorWidth = style.InputTextCursorSize;

        const auto offset = ImGui::GetWindowPos().x;
        m.Draw->PushClipRect(
            ImVec2(offset + m.TextLeftOffset - s.CursorWidth, m.Draw->GetClipRectMin().y),
            ImVec2(offset + m.TextRightOffset + s.CursorWidth, m.Draw->GetClipRectMax().y),
            true
        );
        RenderCurrentLineHighlight(s, m);
        RenderActiveBracketBackground(s, m);
        RenderSelections(s, m);
        RenderMarkers(s, m, m.CursorScreenPos.x + m.TextLeftOffset, ScreenPos(m, {0, s.LastVisibleColumn}).x, &Marker::TextColor, &Marker::TextTooltip);
        RenderMatchingBracketLines(s, m);
        RenderSquiggles(s, m);
        RenderText(s, m);
        RenderCursorCarets(s, m);
        m.Draw->PopClipRect();
        RenderMarkers(s, m, m.CursorScreenPos.x + m.LineNumberLeftOffset, m.CursorScreenPos.x + m.LineNumberRightOffset, &Marker::LineNumberColor, &Marker::LineNumberTooltip);
        RenderLineNumbers(s, m);
        RenderFoldIndicators(s, m);
        RenderMiniMap(s, m);
        RenderScrollbarMiniMap(s, m);
        RenderPanScrollIndicator(s, m);
        RenderFindReplace(s, m);
        RenderAutoComplete(s, m);

        HandlePossibleScrolling(s, m);

        if (ImGui::GetIO().ConfigFlags & (ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad)) {
            // Upstream's workaround: a child window only draws a navigation border when it has a scrollbar.
            auto *window = ImGui::GetCurrentWindow();
            if (ImGui::IsWindowFocused()) {
                if (ImGui::GetCurrentContext()->NavCursorVisible && !window->ScrollbarY) {
                    ImRect bb{ImGui::GetWindowPos(), ImGui::GetWindowPos() + ImGui::GetWindowSize()};
                    bb.Expand(ImVec2(2.0f, 2.0f));
                    window->ParentWindow->DrawList->AddRect(bb.Min, bb.Max, ImGui::GetColorU32(ImGuiCol_NavCursor), ImGui::GetStyle().FrameRounding, m.FontScaleDpi);
                }
            } else {
                window->DC.NavLayersActiveMaskNext |= (1 << window->DC.NavLayerCurrent);
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    return s;
}
} // namespace ipte
