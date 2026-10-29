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

namespace ipte {
namespace {
ImU32 Shade(const State &s, Color c) { return s.Shaded[size_t(c)]; }

struct Metrics {
    ImDrawList *Draw;
    ImFont *Font;
    float FontSize, FontScaleDpi;
    ImVec2 GlyphSize, CursorScreenPos, VisibleSize, TextSize;
    float LineNumberLeftOffset, LineNumberRightOffset, DecorationOffset, FoldIndicatorOffset, TextLeftOffset, TextRightOffset, MiniMapOffset, MiniMapWidth;
    float MiniMapRowHeight, MiniMapColumnHeight, MiniMapColumnWidth;
};
constexpr float MiniMapAlpha = 0.45f, MiniMapViewPortAlpha = 0.15f, MiniMapViewPortActiveAlpha = 0.3f;

void ScrollToLine(State &s, uint64_t line, State::Align alignment, float fraction = 0.0f) {
    s.ScrollToDocPos = {InvalidLine, 0};
    s.ScrollToLineNumber = NormalizeLine(s.Doc, line);
    s.ScrollToAlignment = alignment;
    s.ScrollToFraction = fraction;
    if (s.Config.LineFolding) UnfoldAroundLine(s, line);
}

void FlushScroll(State &s) {
    if (s.ScrollTarget.x < FLT_MAX) ImGui::SetScrollX(s.ScrollTarget.x);
    if (s.ScrollTarget.y < FLT_MAX) ImGui::SetScrollY(s.ScrollTarget.y);
    s.ScrollTarget = ImVec2(FLT_MAX, FLT_MAX);
}

void CompressMarkers(State &s) {
    if (s.Markers.empty()) return;
    std::vector<uint64_t> index(s.Markers.size(), 0);
    for (const auto m : s.LineMarkers)
        if (m) index[m - 1] = 1;
    uint64_t next = 1;
    for (auto &i : index)
        if (i) i = next++;
    for (auto &m : s.LineMarkers)
        if (m) m = index[m - 1];
    std::vector<Marker> kept;
    for (size_t i = 0; i < s.Markers.size(); ++i)
        if (index[i]) kept.push_back(s.Markers[i]);
    s.Markers = std::move(kept);
}

void CompressSquiggles(State &s) {
    if (s.Squiggles.empty()) return;
    std::vector<uint64_t> index(s.Squiggles.size(), 0);
    vec::ForEach(s.GlyphSquiggles, [&](uint64_t id) {
        if (id) index[id - 1] = 1;
    });
    uint64_t next = 1;
    for (auto &i : index)
        if (i) i = next++;
    auto t = s.GlyphSquiggles.transient();
    for (uint64_t i = 0; i < s.GlyphSquiggles.Size; ++i)
        if (const auto id = s.GlyphSquiggles[i]) t.set(i, index[id - 1]);
    s.GlyphSquiggles = std::move(t).persistent();
    std::vector<Squiggle> kept;
    for (size_t i = 0; i < s.Squiggles.size(); ++i)
        if (index[i]) kept.push_back(s.Squiggles[i]);
    s.Squiggles = std::move(kept);
}

uint64_t MarkerAt(const State &s, uint64_t line) { return line < s.LineMarkers.size() ? s.LineMarkers[line] : 0; }
uint64_t SquiggleAt(const State &s, uint64_t offset) { return offset < s.GlyphSquiggles.Size ? s.GlyphSquiggles[offset] : 0; }

// Returns whether the document changed since the last call.
bool UpdateState(State &s, const Metrics &m) {
    const auto bracketsChanged = Refresh(s);
    const auto foldsUpdated = RefreshFolds(s, bracketsChanged);
    UpdateCursors(s);

    float unused;
    const auto firstVisibleLineFraction = std::modf(s.Scroll.y / m.GlyphSize.y, &unused);
    const auto previousFirstLine = VisToDoc(s, {s.FirstVisibleRow, 0}).Line;
    const bool configChanged = s.TypesetTabSize != s.Config.TabSize || s.TypesetWordWrap != s.Config.WordWrap || (s.TypesetWordWrap && s.TypesetWordWrapColumns != s.WordWrapColumns);
    if (configChanged) {
        s.TypesetTabSize = s.Config.TabSize;
        s.TypesetWordWrap = s.Config.WordWrap;
        s.TypesetWordWrapColumns = s.WordWrapColumns;
    }
    if (configChanged || s.DocUpdated || foldsUpdated) {
        RefreshLayout(s);
        if (s.ScrollToLineNumber == InvalidLine && s.ScrollToDocPos.Line == InvalidLine) ScrollToLine(s, previousFirstLine, State::Align::Top, firstVisibleLineFraction);
    }
    RefreshLayout(s);

    const bool documentChanged = s.DocUpdated;
    s.DocUpdated = false;
    if (s.DeletesHappened) {
        CompressMarkers(s);
        CompressSquiggles(s);
        s.DeletesHappened = false;
    }

    RefreshMiniMap(s);

    const auto width = m.TextLeftOffset + s.Layout.TotalColumns * m.GlyphSize.x + s.CursorWidth + (s.Config.ShowMiniMap ? m.MiniMapWidth : 0.0f);
    const auto height = s.Layout.TotalRows * m.GlyphSize.y;
    s.TotalSize = ImVec2(width, height);
    return documentChanged;
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
    if (s.SetClipboard) {
        ImGui::SetClipboardText(s.ClipboardOut.c_str());
        s.SetClipboard = false;
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

    if (sc(ImGuiMod_Ctrl | ImGuiKey_UpArrow)) return Action{Op::MoveToTop, false};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_Home)) return Action{Op::MoveToTop, false};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_UpArrow)) return Action{Op::MoveToTop, true};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Home)) return Action{Op::MoveToTop, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_DownArrow)) return Action{Op::MoveToBottom, false};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_End)) return Action{Op::MoveToBottom, false};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_DownArrow)) return Action{Op::MoveToBottom, true};
    if (sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_End)) return Action{Op::MoveToBottom, true};

    if (sc(ImGuiKey_Home)) return Action{Op::MoveToStartOfLine, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_Home)) return Action{Op::MoveToStartOfLine, true};
    if (sc(ImGuiKey_End)) return Action{Op::MoveToEndOfLine, false};
    if (sc(ImGuiMod_Shift | ImGuiKey_End)) return Action{Op::MoveToEndOfLine, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_A)) return Action{Op::SelectAll};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiKey_D)) return Action{Op::AddNextOccurrence, false, false};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_D)) return Action{Op::AddNextOccurrence, false, true};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_L)) return Action{Op::AddNextOccurrence, false, true};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_D)) return Action{Op::SelectAllOccurrences, false, false};
    if (hasSelection && sc(ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_D)) return Action{Op::SelectAllOccurrences, false, true};

    if (sc(ImGuiMod_Ctrl | ImGuiKey_X)) return Action{Op::Cut};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_Delete)) return Action{Op::Cut};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_C)) return Action{Op::Copy};
    if (sc(ImGuiMod_Ctrl | ImGuiKey_Insert)) return Action{Op::Copy};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_V, R)) return Action{Op::Paste};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_Insert, R)) return Action{Op::Paste};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_Z, R)) return Action{Op::Undo};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, R)) return Action{Op::Redo};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_Y, R)) return Action{Op::Redo};

    if (!ro && sc(ImGuiKey_Delete, R)) return Action{Op::Delete, false, false};
    if (!ro && sc(ImGuiMod_Shift | ImGuiKey_Delete, R)) return Action{Op::Delete, false, false};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_Delete, R)) return Action{Op::Delete, false, true};
    if (!ro && sc(ImGuiKey_Backspace, R)) return Action{Op::Backspace, false, false};
    if (!ro && sc(ImGuiMod_Shift | ImGuiKey_Backspace, R)) return Action{Op::Backspace, false, false};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_Backspace, R)) return Action{Op::Backspace, false, true};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K)) return Action{Op::RemoveSelectedLines};

    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_LeftBracket, R)) return Action{Op::DeindentLines};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_RightBracket, R)) return Action{Op::IndentLines};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_UpArrow)) return Action{Op::MoveUpLines};
    if (!ro && sc(ImGuiMod_Alt | ImGuiKey_DownArrow)) return Action{Op::MoveDownLines};

    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiKey_F)) return Action{Op::OpenFindReplace};
    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F)) return Action{Op::FindAll};
    if (s.Config.FindReplaceEnabled && sc(ImGuiMod_Ctrl | ImGuiKey_G, R)) return Action{Op::FindNext};

    if (!ro && sc(s.Config.AutoComplete.TriggerShortcut)) return Action{Op::AutoCompleteShortcut};

    if (sc(ImGuiKey_Insert)) return Action{Op::ToggleOverwrite};

    if (!ro && sc(ImGuiKey_Enter, R)) return Action{Op::Character, false, false, 0, '\n'};
    if (!ro && sc(ImGuiKey_KeypadEnter, R)) return Action{Op::Character, false, false, 0, '\n'};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_Enter, R)) return Action{Op::InsertLineBelow};
    if (!ro && sc(ImGuiMod_Ctrl | ImGuiKey_KeypadEnter, R)) return Action{Op::InsertLineBelow};
    if (!ro && sc(ImGuiMod_Shift | ImGuiKey_Enter, R)) return Action{Op::InsertLineAbove};
    if (!ro && sc(ImGuiMod_Shift | ImGuiKey_KeypadEnter, R)) return Action{Op::InsertLineAbove};

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
    for (const auto key : {ImGuiKey_Tab, ImGuiKey_Enter, ImGuiKey_KeypadEnter, ImGuiKey_UpArrow, ImGuiKey_DownArrow})
        if (ImGui::IsKeyPressed(key)) return true;
    return false;
}

void HandleKeyboardInputs(State &s, const Metrics &m) {
    if (!ImGui::IsWindowFocused()) return;
    auto &io = ImGui::GetIO();

    // The autocomplete popup handles these keys while it has suggestions.
    if (s.AutoComplete.Active && IsSpecialKeyPressed()) {
        if (!s.AutoComplete.Suggestions.empty()) return;
        Apply(s, Action{Op::AutoCompleteCancel});
    }

    if (auto action = KeyboardAction(s)) {
        if (action->Kind == Op::Paste) {
            const auto *clipboard = ImGui::GetClipboardText();
            if (!clipboard) action.reset();
            else action->Text = clipboard;
        }
        if (action) Apply(s, *action);
    }

    if (!io.InputQueueCharacters.empty()) {
        if (!(ImGui::IsKeyDown(ImGuiMod_Ctrl) && !ImGui::IsKeyDown(ImGuiMod_Alt)) && !s.Config.ReadOnly) {
            for (const auto character : io.InputQueueCharacters) {
                if (character == '\n' || character >= 32) Apply(s, Action{Op::Character, false, false, 0, character});
            }
        }
        io.InputQueueCharacters.resize(0);
    }

    if (!s.Config.ReadOnly) {
        auto *context = ImGui::GetCurrentContext();
        context->PlatformImeData.WantVisible = true;
        context->PlatformImeData.WantTextInput = true;
        const auto vis = DocToVis(s, MainCursor(s.Cursors).End);
        const ImVec2 cursorPos(m.CursorScreenPos.x + m.TextLeftOffset + vis.Column * m.GlyphSize.x, m.CursorScreenPos.y + vis.Row * m.GlyphSize.y);
        context->PlatformImeData.InputPos = ImVec2(cursorPos.x - 1.0f, cursorPos.y);
        context->PlatformImeData.InputLineHeight = m.GlyphSize.y;
        context->PlatformImeData.ViewportId = ImGui::GetCurrentWindow()->Viewport->ID;
    }
    ImGui::SetNextFrameWantCaptureKeyboard(true);
}

void HandleMouseInteractions(State &s, const Metrics &m) {
    auto io = ImGui::GetIO();
    const auto mousePos = ImGui::GetMousePos() - m.CursorScreenPos;
    const auto absoluteMousePos = ImGui::GetMousePos() - ImGui::GetWindowPos();

    const auto overLineNumbers = s.Config.ShowLineNumbers && absoluteMousePos.x > m.LineNumberLeftOffset && absoluteMousePos.x < m.LineNumberRightOffset;
    const auto overMiniMap = s.Config.ShowMiniMap && absoluteMousePos.x > m.MiniMapOffset;
    const auto overText = mousePos.x - s.Scroll.x > m.TextLeftOffset && mousePos.x - s.Scroll.x < m.TextRightOffset && mousePos.y - s.Scroll.y >= 0 && mousePos.y - s.Scroll.y < m.TextSize.y;

    DocPos glyphPos, cursorPos;
    ScreenToDoc(s, ImVec2((mousePos.x - m.TextLeftOffset) / m.GlyphSize.x, mousePos.y / m.GlyphSize.y), glyphPos, cursorPos);

    s.Panning &= s.Config.PanMode && ImGui::IsMouseDown(ImGuiMouseButton_Middle);

    if (s.Panning && ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
        const auto windowSize = ImGui::GetWindowSize();
        auto mouseDelta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
        const float dragFactor = ImGui::GetIO().DeltaTime * 15.0f;
        const ImVec2 autoPanMargin(m.GlyphSize.x * 4.0f, m.GlyphSize.y * 2.0f);
        if (absoluteMousePos.x < m.TextLeftOffset + autoPanMargin.x) mouseDelta.x = (absoluteMousePos.x - (m.TextLeftOffset + autoPanMargin.x)) * dragFactor;
        else if (absoluteMousePos.x > windowSize.x - autoPanMargin.x) mouseDelta.x = (absoluteMousePos.x - (windowSize.x - autoPanMargin.x)) * dragFactor;
        if (absoluteMousePos.y < autoPanMargin.y) mouseDelta.y = (absoluteMousePos.y - autoPanMargin.y) * dragFactor;
        else if (absoluteMousePos.y > windowSize.y - autoPanMargin.y) mouseDelta.y = (absoluteMousePos.y - (windowSize.y - autoPanMargin.y)) * dragFactor;
        Apply(s, Action{.Kind = Op::PanBy, .Vec = mouseDelta});
        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
    } else if (s.Scrolling) {
        const float deadzone = m.GlyphSize.x;
        auto offset = s.ScrollStart - absoluteMousePos;
        offset.x = offset.x < 0.0f ? std::min(offset.x + deadzone, 0.0f) : std::max(offset.x - deadzone, 0.0f);
        offset.y = offset.y < 0.0f ? std::min(offset.y + deadzone, 0.0f) : std::max(offset.y - deadzone, 0.0f);
        offset *= ImGui::GetIO().DeltaTime * 5.0f;
        Apply(s, Action{.Kind = Op::ScrollModeBy, .Vec = offset});
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)) Apply(s, Action{Op::StopScrollMode});
    } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        io.WantCaptureMouse = true;
        if (s.MiniMapIsScrollbar) Apply(s, Action{.Kind = Op::MiniMapDrag, .Vec = ImVec2(m.TextSize.y, absoluteMousePos.y)});
        else if (s.SelectingText && overLineNumbers) Apply(s, Action{.Kind = Op::DragLineSelect, .Pos = cursorPos});
        else if (s.SelectingText && overText) Apply(s, Action{.Kind = Op::DragSelect, .Pos = cursorPos});
    } else if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        Apply(s, Action{Op::MouseReleased});
    } else if (ImGui::IsWindowHovered()) {
        if (ImGui::IsWindowFocused() && overText) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            if (s.Config.PanMode) Apply(s, Action{Op::BeginPan});
            else Apply(s, Action{.Kind = Op::BeginScrollMode, .Vec = absoluteMousePos});
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
            } else {
                const auto extendCursor = ImGui::IsKeyDown(ImGuiMod_Shift);
                const auto addCursor = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGui::IsKeyDown(ImGuiMod_Alt) : ImGui::IsKeyDown(ImGuiMod_Ctrl);
                if (overLineNumbers) {
                    Apply(s, Action{.Kind = Op::ClickLineNumbers, .Select = extendCursor, .Word = addCursor, .Pos = cursorPos});
                } else if (overText) {
                    Apply(s, Action{.Kind = Op::ClickText, .Select = extendCursor, .Word = addCursor, .Pos = cursorPos});
                } else if (overMiniMap) {
                    if (ImGui::GetCurrentWindow()->ScrollbarY) {
                        const auto clickedRow = s.FirstMiniMapRow + uint64_t(absoluteMousePos.y / m.MiniMapRowHeight);
                        const auto inView = clickedRow >= s.FirstVisibleRow && clickedRow <= s.LastVisibleRow;
                        Apply(s, Action{.Kind = Op::ClickMiniMap, .Select = inView, .Count = VisToDoc(s, {clickedRow, 0}).Line, .Vec = ImVec2(0, absoluteMousePos.y)});
                    }
                }
            }
        }

        bool anyMouseDown = false;
        for (int i = 0; !anyMouseDown && i < int(ImGuiMouseButton_Middle); ++i)
            if (ImGui::IsMouseDown(ImGuiMouseButton(i))) anyMouseDown = true;
        if (anyMouseDown) Apply(s, Action{Op::MouseDown});
    }
}

void RenderCurrentLineHighlight(const State &s, const Metrics &m) {
    if (!s.Config.ShowCurrentLineHighlight || AnyHasSelection(s.Cursors)) return;
    std::vector<uint64_t> drawn;
    for (const auto &c : s.Cursors.Items) {
        const auto line = c.End.Line;
        if (std::ranges::find(drawn, line) != drawn.end()) continue;
        drawn.push_back(line);
        const auto row = LineAt(s, line).Row, rows = LineAt(s, line).Rows;
        // Upstream omits the window x position here. See UpstreamBugs.md.
        const auto topLeft = ImVec2(m.TextLeftOffset, m.CursorScreenPos.y + row * m.GlyphSize.y);
        const auto bottomRight = topLeft + ImVec2(m.TextRightOffset, rows * m.GlyphSize.y);
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
        if (vis.Row >= s.FirstVisibleRow && vis.Row <= s.LastVisibleRow) {
            const auto x = m.CursorScreenPos.x + m.TextLeftOffset + vis.Column * m.GlyphSize.x;
            const auto y = m.CursorScreenPos.y + vis.Row * m.GlyphSize.y;
            m.Draw->AddRectFilled(ImVec2(x, y), ImVec2(x + m.GlyphSize.x, y + m.GlyphSize.y), Shade(s, Color::MatchingBracketBackground));
        }
    }
}

void RenderMatchingBracketLines(const State &s, const Metrics &m) {
    if (!s.Config.ShowMatchingBrackets || s.Derived.Brackets.empty()) return;
    const auto *active = EnclosingBrackets(s.Derived, MainCursor(s.Cursors).End);
    for (const auto &bracket : s.Derived.Brackets) {
        if (!bracket.Visible || bracket.End.Line - bracket.Start.Line <= 1) continue;
        const auto column = std::min(DocToVis(s, bracket.Start).Column, DocToVis(s, bracket.End).Column);
        for (auto i = bracket.Start.Line + 1; i < bracket.End.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto row = LineAt(s, i).Row, rows = LineAt(s, i).Rows;
            const auto lineX = m.CursorScreenPos.x + m.TextLeftOffset + column * m.GlyphSize.x;
            const auto startY = m.CursorScreenPos.y + row * m.GlyphSize.y;
            const auto endY = startY + rows * m.GlyphSize.y;
            const auto color = Shade(s, &bracket == active ? Color::MatchingBracketActive : Color::Whitespace);
            m.Draw->AddLine(ImVec2(lineX, startY), ImVec2(lineX, endY), color);
        }
    }
}

void RenderSelections(const State &s, const Metrics &m) {
    for (const auto &c : s.Cursors.Items) {
        if (!c.HasSelection()) continue;
        const auto begin = c.SelectionStart(), end = c.SelectionEnd();
        for (auto i = begin.Line; i <= end.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto &line = LineAt(s, i);
            const auto x = m.CursorScreenPos.x + m.TextLeftOffset;
            if (line.Rows == 1) {
                if (line.Row < s.FirstVisibleRow || line.Row > s.LastVisibleRow) continue;
                const DocPos lineLeft{i, 0}, lineRight{i, LineLength(s.Doc, i)};
                const auto docLeft = begin <= lineLeft ? lineLeft : begin;
                const auto docRight = end > lineRight ? lineRight : end;
                const auto visLeft = DocToVis(s, docLeft), visRight = DocToVis(s, docRight);
                const auto left = x + visLeft.Column * m.GlyphSize.x, right = x + visRight.Column * m.GlyphSize.x;
                const auto y = m.CursorScreenPos.y + line.Row * m.GlyphSize.y;
                m.Draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + m.GlyphSize.y), Shade(s, Color::Selection));
            } else {
                for (uint64_t j = 0; j < line.Rows; ++j) {
                    const auto &section = s.Layout.Sections[line.FirstSection + j];
                    const auto row = line.Row + j;
                    if (row < s.FirstVisibleRow || row > s.LastVisibleRow) continue;
                    const DocPos sectionLeft{i, section.StartIndex}, sectionRight{i, section.EndIndex};
                    if (!(begin < sectionRight && end > sectionLeft)) continue;
                    const auto visLeft = begin <= sectionLeft ? VisPos{row, 0} : DocToVis(s, begin);
                    const auto visRight = end > sectionRight ? VisPos{row, section.Columns} : DocToVis(s, end);
                    const auto left = x + visLeft.Column * m.GlyphSize.x, right = x + visRight.Column * m.GlyphSize.x;
                    const auto y = m.CursorScreenPos.y + row * m.GlyphSize.y;
                    m.Draw->AddRectFilled(ImVec2(left, y), ImVec2(right, y + m.GlyphSize.y), Shade(s, Color::Selection));
                }
            }
        }
    }
}

void RenderTextMarkers(const State &s, const Metrics &m) {
    if (s.Markers.empty()) return;
    for (auto row = s.FirstVisibleRow; row <= s.LastVisibleRow; ++row) {
        const auto markerIndex = MarkerAt(s, s.Layout.Rows[row].Line);
        if (!markerIndex) continue;
        const auto &marker = s.Markers[markerIndex - 1];
        const auto y = m.CursorScreenPos.y + row * m.GlyphSize.y;
        if (((marker.TextColor >> IM_COL32_A_SHIFT) & 0xFF) == 0) continue;
        const auto left = m.CursorScreenPos.x + m.TextLeftOffset;
        const auto right = left + s.LastVisibleColumn * m.GlyphSize.x;
        const auto start = ImVec2(left, y), end = ImVec2(right, y + m.GlyphSize.y);
        m.Draw->AddRectFilled(start, end, marker.TextColor);
        if (!marker.TextTooltip.empty() && ImGui::IsMouseHoveringRect(start, end)) {
            if (ImGui::BeginTooltip()) {
                ImGui::PushStyleColor(ImGuiCol_PopupBg, marker.TextColor);
                ImGui::TextUnformatted(marker.TextTooltip.c_str());
                ImGui::PopStyleColor();
                ImGui::EndTooltip();
            }
        }
    }
}

void RenderSquiggle(ImDrawList *drawList, float left, float right, float top, float bottom, float thickness, ImU32 color, const char *tooltip, SquiggleStyle style) {
    const ImVec2 topLeft{left, top}, bottomRight{right, bottom};
    switch (style) {
        case SquiggleStyle::Wave: {
            const auto height = bottom - top;
            const auto size = height * 0.2f;
            const auto offset = top + height * 0.8f;
            ImVec2 point(left, offset);
            bool down = true;
            drawList->PushClipRect(topLeft, bottomRight, true);
            while (point.x < right) {
                const ImVec2 next{point.x + size, down ? offset + size : offset};
                drawList->AddLine(point, next, color, thickness);
                point = next;
                down = !down;
            }
            drawList->PopClipRect();
            break;
        }
        case SquiggleStyle::Background: drawList->AddRectFilled(topLeft, bottomRight, color); break;
    }
    if (*tooltip && ImGui::IsMouseHoveringRect(topLeft, bottomRight)) {
        if (ImGui::BeginTooltip()) {
            ImGui::TextUnformatted(tooltip);
            ImGui::EndTooltip();
        }
    }
}

void RenderSquiggles(const State &s, const Metrics &m) {
    if (s.Squiggles.empty()) return;
    ImVec2 rowScreenPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset, s.FirstVisibleRow * m.GlyphSize.y);
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        const auto &line = LineAt(s, row.Line);
        const auto lineStart = LineStart(s.Doc, row.Line);
        uint64_t index = 0, column = 0, endColumn = line.Columns, lineColumn = 0;
        if (s.Config.WordWrap && line.FirstSection != InvalidLine) {
            const auto &section = s.Layout.Sections[line.FirstSection + row.Section];
            index = section.StartIndex;
            column = section.Indent;
            endColumn = section.Columns;
            for (uint64_t k = 0; k < index; ++k) lineColumn += Columns(s.Config, s.Doc[lineStart + k], lineColumn);
        }
        bool inSquiggle = false;
        size_t squiggleIndex = 0;
        float squiggleLeft = 0.0f;
        const float thickness = 1.2f * m.FontScaleDpi;
        while (column < endColumn && column <= s.LastVisibleColumn) {
            const auto offset = lineStart + index++;
            const auto columns = Columns(s.Config, s.Doc[offset], lineColumn);
            lineColumn += columns;
            const ImVec2 glyphPos(rowScreenPos.x + column * m.GlyphSize.x, rowScreenPos.y);
            if (const auto id = SquiggleAt(s, offset)) {
                const auto nextIndex = id - 1;
                if (inSquiggle) {
                    if (squiggleIndex != nextIndex) {
                        const auto &squiggle = s.Squiggles[squiggleIndex];
                        RenderSquiggle(m.Draw, squiggleLeft, glyphPos.x, glyphPos.y, glyphPos.y + m.GlyphSize.y, thickness, squiggle.Shade, squiggle.Tooltip.c_str(), squiggle.Style);
                        squiggleIndex = nextIndex;
                        squiggleLeft = glyphPos.x;
                    }
                } else {
                    inSquiggle = true;
                    squiggleIndex = nextIndex;
                    squiggleLeft = glyphPos.x;
                }
            } else if (inSquiggle) {
                const auto &squiggle = s.Squiggles[squiggleIndex];
                RenderSquiggle(m.Draw, squiggleLeft, glyphPos.x, glyphPos.y, glyphPos.y + m.GlyphSize.y, thickness, squiggle.Shade, squiggle.Tooltip.c_str(), squiggle.Style);
                inSquiggle = false;
            }
            column += columns;
        }
        if (inSquiggle) {
            const auto &squiggle = s.Squiggles[squiggleIndex];
            const auto glyphPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset + row.Columns * m.GlyphSize.x, i * m.GlyphSize.y);
            RenderSquiggle(m.Draw, squiggleLeft, glyphPos.x, glyphPos.y, glyphPos.y + m.GlyphSize.y, thickness, squiggle.Shade, squiggle.Tooltip.c_str(), squiggle.Style);
        }
        rowScreenPos.y += m.GlyphSize.y;
    }
}

void RenderLineNumberMarkers(const State &s, const Metrics &m) {
    if (s.Markers.empty()) return;
    for (auto row = s.FirstVisibleRow; row <= s.LastVisibleRow; ++row) {
        const auto markerIndex = MarkerAt(s, s.Layout.Rows[row].Line);
        if (!markerIndex) continue;
        const auto &marker = s.Markers[markerIndex - 1];
        const auto y = m.CursorScreenPos.y + row * m.GlyphSize.y;
        if (((marker.LineNumberColor >> IM_COL32_A_SHIFT) & 0xFF) == 0) continue;
        const auto left = m.CursorScreenPos.x + m.LineNumberLeftOffset, right = m.CursorScreenPos.x + m.LineNumberRightOffset;
        const auto start = ImVec2(left, y), end = ImVec2(right, y + m.GlyphSize.y);
        m.Draw->AddRectFilled(start, end, marker.LineNumberColor);
        if (!marker.LineNumberTooltip.empty() && ImGui::IsMouseHoveringRect(start, end)) {
            if (ImGui::BeginTooltip()) {
                ImGui::PushStyleColor(ImGuiCol_PopupBg, marker.LineNumberColor);
                ImGui::TextUnformatted(marker.LineNumberTooltip.c_str());
                ImGui::PopStyleColor();
                ImGui::EndTooltip();
            }
        }
    }
}

ImU32 MarkerMiniColor(const Marker &marker) { return marker.TextColor ? marker.TextColor : marker.LineNumberColor; }

void RenderText(const State &s, const Metrics &m) {
    ImVec2 rowScreenPos = m.CursorScreenPos + ImVec2(m.TextLeftOffset, s.FirstVisibleRow * m.GlyphSize.y);
    const auto firstRenderableColumn = (s.FirstVisibleColumn / s.Config.TabSize) * s.Config.TabSize;
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        const auto &line = LineAt(s, row.Line);
        const auto lineStart = LineStart(s.Doc, row.Line);
        uint64_t index = 0, column = 0, endColumn = line.Columns, lineColumn = 0;
        if (s.Config.WordWrap && line.FirstSection != InvalidLine) {
            const auto &section = s.Layout.Sections[line.FirstSection + row.Section];
            index = section.StartIndex;
            column = section.Indent;
            endColumn = section.Columns;
            for (uint64_t k = 0; k < index; ++k) lineColumn += Columns(s.Config, s.Doc[lineStart + k], lineColumn);
        }
        while (column < endColumn && column <= s.LastVisibleColumn) {
            const auto cp = s.Doc[lineStart + index];
            const auto color = ColorAt(s, lineStart + index++);
            const auto columns = Columns(s.Config, cp, lineColumn);
            lineColumn += columns;
            const ImVec2 glyphPos(rowScreenPos.x + column * m.GlyphSize.x, rowScreenPos.y);
            if (cp == '\t') {
                if (s.Config.ShowTabs && column >= firstRenderableColumn) {
                    const auto x1 = glyphPos.x + m.GlyphSize.x * 0.3f;
                    const auto y = glyphPos.y + m.FontSize * 0.5f;
                    const auto x2 = glyphPos.x + m.GlyphSize.x;
                    const ImVec2 p1(x1, y), p2(x2, y), p3(x2 - m.FontSize * 0.16f, y - m.FontSize * 0.16f), p4(x2 - m.FontSize * 0.16f, y + m.FontSize * 0.16f);
                    m.Draw->AddLine(p1, p2, Shade(s, Color::Whitespace));
                    m.Draw->AddLine(p2, p3, Shade(s, Color::Whitespace));
                    m.Draw->AddLine(p2, p4, Shade(s, Color::Whitespace));
                }
            } else if (cp == ' ') {
                if (s.Config.ShowSpaces && column >= firstRenderableColumn) {
                    const auto x = glyphPos.x + m.GlyphSize.x * 0.5f;
                    const auto y = glyphPos.y + m.FontSize * 0.5f;
                    m.Draw->AddCircleFilled(ImVec2(x, y), 1.5f, Shade(s, Color::Whitespace), 4);
                }
            } else if (column >= firstRenderableColumn) {
                m.Font->RenderChar(m.Draw, m.FontSize, glyphPos, Shade(s, color), ImWchar(cp));
            }
            column += columns;
        }
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
    for (const auto &c : s.Cursors.Items) {
        if (IsHidden(s, c.End.Line)) continue;
        const auto pos = DocToVis(s, c.End);
        if (pos.Row >= s.FirstVisibleRow && pos.Row <= s.LastVisibleRow && pos.Column >= s.FirstVisibleColumn && pos.Column <= s.LastVisibleColumn) {
            const auto caretVisible = !io.ConfigInputTextCursorBlink || s.CaretTimer <= 0.0f || std::fmod(s.CaretTimer, 1.2f) <= 0.8f;
            const auto x = m.CursorScreenPos.x + m.TextLeftOffset + pos.Column * m.GlyphSize.x;
            const auto y = m.CursorScreenPos.y + pos.Row * m.GlyphSize.y;
            if (caretVisible) m.Draw->AddRectFilled(ImVec2(x - s.CursorWidth, y), ImVec2(x, y + m.GlyphSize.y), Shade(s, Color::Cursor));
        }
    }
}

void RenderMiniMap(State &s, const Metrics &m) {
    if (!s.Config.ShowMiniMap) return;
    std::vector<ImU32> rowColors(s.MiniRows.size(), 0);
    for (const auto &c : s.Cursors.Items) {
        const auto begin = c.SelectionStart(), end = c.SelectionEnd();
        for (auto i = begin.Line; i <= end.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto &line = LineAt(s, i);
            for (uint64_t j = 0; j < line.Rows; ++j) rowColors[line.Row + j] = s.MiniShaded[size_t(Color::Selection)];
        }
    }
    if (!s.Markers.empty()) {
        for (uint64_t row = 0; row < RowCount(s); ++row) {
            if (const auto markerIndex = MarkerAt(s, s.Layout.Rows[row].Line)) {
                auto fColor = ImGui::ColorConvertU32ToFloat4(MarkerMiniColor(s.Markers[markerIndex - 1]));
                fColor.w *= MiniMapAlpha;
                rowColors[row] = ImGui::ColorConvertFloat4ToU32(fColor);
            }
        }
    }
    const auto totalMiniMapRows = s.MiniRows.size();
    const auto visibleMiniMapRows = m.TextSize.y / m.MiniMapRowHeight;
    if (totalMiniMapRows < visibleMiniMapRows) {
        s.FirstMiniMapRow = 0;
        s.LastMiniMapRow = s.MiniRows.size();
    } else {
        const auto scrollRatio = ImGui::GetScrollY() / ImGui::GetScrollMaxY();
        s.FirstMiniMapRow = uint64_t(scrollRatio * (totalMiniMapRows - visibleMiniMapRows));
        s.LastMiniMapRow = std::min(s.FirstMiniMapRow + uint64_t(std::ceil(visibleMiniMapRows)), uint64_t(totalMiniMapRows));
    }
    auto pos = ImGui::GetWindowPos() + ImVec2(m.MiniMapOffset, 0.0f);
    for (auto i = s.FirstMiniMapRow; i < s.LastMiniMapRow; ++i) {
        if (rowColors[i]) m.Draw->AddRectFilled(pos, pos + ImVec2(m.MiniMapWidth, m.MiniMapRowHeight), rowColors[i]);
        for (const auto &section : s.MiniRows[i].Sections)
            m.Draw->AddRectFilled(pos + ImVec2(section.Start * m.MiniMapColumnWidth, 0.0f), pos + ImVec2(section.End * m.MiniMapColumnWidth, m.MiniMapColumnHeight), s.MiniShaded[size_t(section.Shade)]);
        pos += ImVec2(0.0f, m.MiniMapRowHeight);
    }
    if (s.TotalSize.y > m.TextSize.y) {
        const auto viewPortStart = (s.FirstVisibleRow - s.FirstMiniMapRow) * m.MiniMapRowHeight;
        const auto viewportHeight = (s.LastVisibleRow - s.FirstVisibleRow) * m.MiniMapRowHeight;
        const auto topLeft = ImGui::GetWindowPos() + ImVec2(m.MiniMapOffset, viewPortStart);
        const auto bottomRight = topLeft + ImVec2(m.MiniMapWidth, viewportHeight);
        auto fColor = ImGui::ColorConvertU32ToFloat4(Shade(s, Color::Text));
        fColor.w *= s.MiniMapIsScrollbar ? MiniMapViewPortActiveAlpha : MiniMapViewPortAlpha;
        m.Draw->AddRectFilled(topLeft, bottomRight, ImGui::ColorConvertFloat4ToU32(fColor));
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
    const auto color = Shade(s, Color::LineNumber), hoveredColor = Shade(s, Color::CurrentLineNumber);
    const auto size = m.FontSize * 0.25f;
    for (auto i = s.FirstVisibleRow; i <= s.LastVisibleRow; ++i) {
        const auto &row = s.Layout.Rows[i];
        const auto foldingState = FoldAt(s, row.Line);
        if (row.Section != 0 || (foldingState != FoldingState::Foldable && foldingState != FoldingState::Folded)) continue;
        const auto center = ImVec2(ImGui::GetWindowPos().x + m.FoldIndicatorOffset, m.CursorScreenPos.y + m.GlyphSize.y * i + m.FontSize / 2.0f);
        const auto topLeft = center - ImVec2(m.GlyphSize.x / 2.0f, m.FontSize / 2.0f);
        const auto bottomRight = center + ImVec2(m.GlyphSize.x / 2.0f, m.FontSize / 2.0f);
        const auto isHovered = ImGui::IsMouseHoveringRect(topLeft, bottomRight);
        if (foldingState == FoldingState::Foldable) {
            m.Draw->AddLine(ImVec2(center.x - size, center.y - size * 0.5f), ImVec2(center.x, center.y + size), isHovered ? hoveredColor : color);
            m.Draw->AddLine(ImVec2(center.x, center.y + size), ImVec2(center.x + size, center.y - size * 0.5f), isHovered ? hoveredColor : color);
        } else {
            m.Draw->AddLine(ImVec2(center.x - size * 0.5f, center.y - size), ImVec2(center.x + size, center.y), isHovered ? hoveredColor : color);
            m.Draw->AddLine(ImVec2(center.x + size, center.y), ImVec2(center.x - size * 0.5f, center.y + size), isHovered ? hoveredColor : color);
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
    const auto offset = (rect.Max.x - rect.Min.x) * 0.3f;
    const auto left = rect.Min.x + offset, right = rect.Max.x - offset;
    m.Draw->PushClipRect(rect.Min, rect.Max, false);
    for (const auto &c : s.Cursors.Items) {
        const auto begin = c.SelectionStart(), end = c.SelectionEnd();
        for (auto i = begin.Line; i <= end.Line; ++i) {
            if (IsHidden(s, i)) continue;
            const auto row = LineAt(s, i).Row, rows = LineAt(s, i).Rows;
            const auto ly1 = std::round(rect.Min.y + row * rowHeight), ly2 = std::round(rect.Min.y + (row + rows) * rowHeight);
            m.Draw->AddRectFilled(ImVec2(left, ly1), ImVec2(right, ly2), Shade(s, Color::Selection));
        }
    }
    if (!s.Markers.empty()) {
        for (uint64_t row = 0; row < RowCount(s); ++row) {
            if (const auto markerIndex = MarkerAt(s, s.Layout.Rows[row].Line)) {
                const auto ly = std::round(rect.Min.y + row * rowHeight);
                m.Draw->AddRectFilled(ImVec2(left, ly), ImVec2(right, ly + rowHeight), MarkerMiniColor(s.Markers[markerIndex - 1]));
            }
        }
    }
    m.Draw->PopClipRect();
}

bool LatchButton(const char *label, bool value, ImVec2 size) {
    const auto *colors = ImGui::GetStyle().Colors;
    if (value) {
        ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_ButtonActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_ButtonActive]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_TableBorderLight]);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, colors[ImGuiCol_TableBorderLight]);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colors[ImGuiCol_TableBorderLight]);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, colors[ImGuiCol_ButtonActive]);
    }
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
    const auto fieldWidth = 250.0f * ImGui::GetStyle().FontScaleDpi;
    auto button1Width = ImGui::CalcTextSize(s.Config.FindButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f;
    auto button2Width = ImGui::CalcTextSize(s.Config.FindAllButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f;
    const auto optionWidth = ImGui::CalcTextSize("Aa").x + style.ItemSpacing.x * 2.0f;
    if (!s.Config.ReadOnly) {
        button1Width = std::max(button1Width, ImGui::CalcTextSize(s.Config.ReplaceButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f);
        button2Width = std::max(button2Width, ImGui::CalcTextSize(s.Config.ReplaceAllButtonLabel.c_str()).x + style.ItemSpacing.x * 2.0f);
    }
    const auto windowHeight = style.ChildBorderSize * 2.0f + style.WindowPadding.y * 2.0f + ImGui::GetFrameHeight() + (s.Config.ReadOnly ? 0.0f : (style.ItemSpacing.y + ImGui::GetFrameHeight()));
    const auto windowWidth = style.ChildBorderSize * 2.0f + style.WindowPadding.x * 2.0f + fieldWidth + style.ItemSpacing.x + button1Width + style.ItemSpacing.x + button2Width + style.ItemSpacing.x + optionWidth * 3.0f + style.ItemSpacing.x * 2.0f;
    const auto availableSpace = ImGui::GetWindowWidth() - (s.Config.ShowMiniMap ? m.MiniMapWidth : 0.0f) - (ImGui::GetCurrentWindow()->ScrollbarY ? ImGui::GetStyle().ScrollbarSize : 0.0f);
    ImGui::SetNextWindowPos(ImGui::GetWindowPos() + ImVec2(availableSpace - windowWidth - style.ItemSpacing.x, style.ItemSpacing.y * 2.0f));
    ImGui::SetNextWindowSize(ImVec2(windowWidth, windowHeight));
    ImGui::SetNextWindowBgAlpha(0.75f);

    ImGui::BeginChild("find-replace", ImVec2(windowWidth, windowHeight), ImGuiChildFlags_Borders);
    ImGui::SetNextItemWidth(fieldWidth);
    if (s.FocusOnFind) {
        ImGui::SetKeyboardFocusHere();
        s.FocusOnFind = false;
    } else if (s.FindCancelledAutocomplete) {
        ImGui::SetKeyboardFocusHere();
        s.FindCancelledAutocomplete = false;
    }
    // The field edits a copy. A change is applied as an action.
    auto findText = s.FindText;
    if (InputString("###find", findText, ImGuiInputTextFlags_AutoSelectAll)) Apply(s, Action{.Kind = Op::SelectFirstOccurrence, .Text = findText});
    if (ImGui::IsItemDeactivated()) {
        if (ImGui::IsKeyPressed(ImGuiKey_Escape)) Apply(s, Action{Op::CloseFindReplace});
        else if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) Apply(s, Action{Op::FocusEditor});
    }

    const bool disableFindButtons = s.FindText.empty();
    if (disableFindButtons) ImGui::BeginDisabled();
    ImGui::SameLine();
    if (ImGui::Button(s.Config.FindButtonLabel.c_str(), ImVec2(button1Width, 0.0f))) Apply(s, Action{Op::Find});
    ImGui::SameLine();
    if (ImGui::Button(s.Config.FindAllButtonLabel.c_str(), ImVec2(button2Width, 0.0f))) Apply(s, Action{Op::FindAll});
    if (disableFindButtons) ImGui::EndDisabled();
    ImGui::SameLine();
    if (LatchButton("Aa", s.CaseSensitiveFind, ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::ToggleCaseSensitiveFind});
    ImGui::SameLine();
    if (LatchButton("[]", s.WholeWordFind, ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::ToggleWholeWordFind});
    ImGui::SameLine();
    if (ImGui::Button("x", ImVec2(optionWidth, 0.0f))) Apply(s, Action{Op::CloseFindReplace});

    if (!s.Config.ReadOnly) {
        ImGui::SetNextItemWidth(fieldWidth);
        InputString("###replace", s.ReplaceText);
        ImGui::SameLine();
        const bool disableReplaceButtons = s.FindText.empty() || s.ReplaceText.empty();
        if (disableReplaceButtons) ImGui::BeginDisabled();
        if (ImGui::Button(s.Config.ReplaceButtonLabel.c_str(), ImVec2(button1Width, 0.0f))) Apply(s, Action{Op::Replace});
        ImGui::SameLine();
        if (ImGui::Button(s.Config.ReplaceAllButtonLabel.c_str(), ImVec2(button2Width, 0.0f))) Apply(s, Action{Op::ReplaceAll});
        if (disableReplaceButtons) ImGui::EndDisabled();
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
        auto color = ImGui::GetColorU32(ImGuiCol_Text);
        if (j < term.size() && ToLower(term[j]) == ToLower(cp)) {
            color = ImGui::GetColorU32(ImGuiCol_TextLink);
            ++j;
        }
        font->RenderChar(drawList, fontSize, glyphPos, color, ImWchar(cp));
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
        if (newLocation.Line != ac.CurrentLocation.Line) {
            ac.RequestDeactivation = true;
        } else if (FindWordStart(s.Doc, newLocation, true) == ac.StartLocation) {
            ac.CurrentLocation = newLocation;
            if (ac.CurrentLocation == ac.StartLocation) {
                ac.RequestDeactivation = true;
            } else {
                UpdateAutoCompleteContext(s);
                RefreshSuggestions(s);
            }
        } else {
            ac.RequestDeactivation = true;
        }
    }

    bool result = false;
    const auto pos = DocToVis(s, ac.CurrentLocation);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetCursorScreenPos().x + m.TextLeftOffset + pos.Column * m.GlyphSize.x, ImGui::GetCursorScreenPos().y + (pos.Row + 1) * m.GlyphSize.y));
    const auto suggestions = ac.Suggestions.size();
    if (suggestions == 0 && !ac.TriggeredManually) ac.RequestDeactivation = true;
    const auto visibleSuggestions = suggestions == 0 ? 1 : std::min(size_t(10), suggestions);
    const auto &style = ImGui::GetStyle();
    const auto height = ImGui::GetFrameHeightWithSpacing() * visibleSuggestions + style.WindowPadding.y * 2.0f;
    ImGui::SetNextWindowSize(ImVec2(config.SuggestionWidth * m.GlyphSize.x, height));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
    if (ImGui::BeginPopup("AutoCompleteContextMenu", flags)) {
        if (ImGui::IsWindowAppearing()) ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        if (ac.RequestDeactivation) {
            ImGui::CloseCurrentPopup();
            ac.RequestDeactivation = false;
            ac.Active = false;
        } else if (suggestions) {
            const auto items = ac.Suggestions.size();
            auto scroll = false;
            if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                scroll = true;
                ac.CurrentSelection = ac.CurrentSelection == 0 ? items - 1 : ac.CurrentSelection - 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                scroll = true;
                ac.CurrentSelection = ac.CurrentSelection == items - 1 ? 0 : ac.CurrentSelection + 1;
            } else if (ImGui::IsKeyPressed(ImGuiKey_Tab) || ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter)) {
                ac.RequestDeactivation = true;
                result = true;
            } else if (config.AutoInsertSingleSuggestions && ac.TriggeredManually && items == 1) {
                ac.RequestDeactivation = true;
                result = true;
            }
            for (size_t i = 0; i < items; ++i) {
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
    std::vector<uint64_t> codepoints;
    uint64_t column = 0; // Of the line being read, for tabs turned to spaces.
    for (const auto cp : Codepoints(text)) {
        if (cp == Newline) {
            codepoints.push_back(Newline);
            column = 0;
        } else if (config.InsertSpacesOnTabs && cp == '\t') {
            const auto spaces = ((column / config.TabSize) + 1) * config.TabSize - column;
            for (uint64_t k = 0; k < spaces; ++k) codepoints.push_back(' ');
            column += spaces;
        } else if (cp != '\r') {
            codepoints.push_back(cp);
            column += GlyphWidth(cp);
        }
    }
    State s;
    s.Doc = Text{codepoints.begin(), codepoints.end()};
    s.Config = std::move(config);
    ClearAll(s.Cursors);
    s.DocUpdated = true;
    s.ResetScroll = true;
    s.LineFoldsFor = s.Config.LineFolding;
    return s;
}

State Frame(State s, const char *title, ImVec2 size, ImGuiChildFlags childFlags, ImGuiWindowFlags windowFlags) {
    s.ClockUs += int64_t(double(ImGui::GetIO().DeltaTime) * 1e6);
    if (!s.FirstFrame) {
        if (s.FocusOnEditor) {
            ImGui::SetNextWindowFocus();
            s.FocusOnEditor = false;
        }
    } else {
        s.FirstFrame = false;
    }

    ImGui::SetNextWindowContentSize(s.TotalSize);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImGui::ColorConvertU32ToFloat4(Shade(s, Color::Background)));

    if (ImGui::BeginChild(title, size, childFlags, windowFlags)) {
        const ImVec2 current(ImGui::GetScrollX(), ImGui::GetScrollY());
        if (s.ResetScroll) {
            ImGui::SetScrollX(0.0f);
            ImGui::SetScrollY(0.0f);
            s.ResetScroll = false;
        } else if (ImGui::IsWindowAppearing() ? s.Scroll.x != 0.0f || s.Scroll.y != 0.0f : (current.x == s.ScrollSeen.x && current.y == s.ScrollSeen.y && (s.Scroll.x != current.x || s.Scroll.y != current.y))) {
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
        m.VisibleSize = ImGui::GetCurrentWindow()->InnerRect.GetSize();

        if (s.Config.ShowLineNumbers) {
            const auto digits = size_t(std::log10(float(LineCount(s.Doc) + 1)) + 1.0f);
            m.LineNumberLeftOffset = s.Config.LeftMargin * m.GlyphSize.x;
            m.LineNumberRightOffset = m.LineNumberLeftOffset + digits * m.GlyphSize.x;
        } else {
            m.LineNumberLeftOffset = 0.0f;
            m.LineNumberRightOffset = 0.0f;
        }
        if (s.Config.LineFolding) {
            m.FoldIndicatorOffset = m.LineNumberLeftOffset;
            m.LineNumberLeftOffset += m.GlyphSize.x;
            m.LineNumberRightOffset += m.GlyphSize.x;
        }
        m.DecorationOffset = m.LineNumberRightOffset;
        m.TextLeftOffset = m.DecorationOffset + s.Config.TextMargin * m.GlyphSize.x;
        m.MiniMapWidth = 0.0f;
        if (s.Config.ShowMiniMap) {
            m.MiniMapRowHeight = 3.0f * m.FontScaleDpi;
            m.MiniMapColumnHeight = 2.0f * m.FontScaleDpi;
            m.MiniMapColumnWidth = 1.0f * m.FontScaleDpi;
            if (s.Config.MiniMapColumns == 0) m.MiniMapWidth = std::floor((m.VisibleSize.x - m.TextLeftOffset) / (m.GlyphSize.x + m.MiniMapColumnWidth)) * m.MiniMapColumnWidth;
            else m.MiniMapWidth = s.Config.MiniMapColumns * m.MiniMapColumnWidth;
            m.TextRightOffset = m.VisibleSize.x - m.MiniMapWidth;
            m.MiniMapOffset = m.TextRightOffset;
        } else {
            m.TextRightOffset = m.VisibleSize.x;
        }
        m.TextSize = ImVec2(m.TextRightOffset - m.TextLeftOffset, m.VisibleSize.y);
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
        RenderTextMarkers(s, m);
        RenderMatchingBracketLines(s, m);
        RenderSquiggles(s, m);
        RenderText(s, m);
        RenderCursorCarets(s, m);
        m.Draw->PopClipRect();
        RenderLineNumberMarkers(s, m);
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
