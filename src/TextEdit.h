#pragma once

#include "LineBreak.h"
#include "Vector.h"
#include "imgui.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace ipte {
struct Language;

using Text = vec::Weighted;

enum struct Color : uint8_t {
    Text,
    Keyword,
    Declaration,
    Number,
    String,
    Punctuation,
    Preprocessor,
    Identifier,
    KnownIdentifier,
    Comment,
    Background,
    Cursor,
    Selection,
    Whitespace,
    MatchingBracketBackground,
    MatchingBracketActive,
    MatchingBracketLevel1,
    MatchingBracketLevel2,
    MatchingBracketLevel3,
    MatchingBracketError,
    LineNumber,
    CurrentLineNumber,
    CurrentLineHighlight,
    CurrentLineHighlightBorder,
    Count,
};
using Palette = std::array<ImU32, size_t(Color::Count)>;
const Palette &DarkPalette();
const Palette &LightPalette();

struct DocPos {
    uint64_t Line{0}, Index{0};
    auto operator<=>(const DocPos &) const = default;
};

struct AutoCompleteContext {
    std::string SearchTerm;
    DocPos Start, End;
    bool InIdentifier{false}, InNumber{false}, InComment{false}, InString{false};
    const Language *Lang = nullptr;
};

struct AutoCompleteConfig {
    bool Enabled{false};
    bool TriggerOnTyping{true}, TriggerOnShortcut{true}, TriggerInComments{false}, TriggerInStrings{false};
#ifdef __APPLE__
    ImGuiKeyChord TriggerShortcut{ImGuiMod_Super | ImGuiKey_Space};
#else
    ImGuiKeyChord TriggerShortcut{ImGuiMod_Ctrl | ImGuiKey_Space};
#endif
    bool AutoInsertSingleSuggestions{false};
    int64_t TriggerDelayMs{200};
    std::string NoSuggestionsLabel{"No suggestions"};
    uint32_t SuggestionWidth{30};
    std::vector<std::string> (*Provider)(const AutoCompleteContext &){nullptr};
};

struct Settings {
    uint32_t TabSize{4};
    bool InsertSpacesOnTabs{false};
    float LineSpacing{1};
    bool WordWrap{false};
    bool ReadOnly{false};
    bool CaretsVisible{true};
    bool AutoIndent{true};
    bool ShowSpaces{true};
    bool ShowTabs{true};
    bool ShowLineNumbers{true};
    bool ShowMiniMap{false};
    uint32_t MiniMapColumns{0};
    bool ShowScrollbarMiniMap{true};
    bool ShowCurrentLineHighlight{true};
    bool ShowMatchingBrackets{true};
    bool CompletePairedGlyphs{true};
    bool LineFolding{false};
    bool Overwrite{false};
    bool PanMode{true};
    bool ShowPanScrollIndicator{true};
    bool FindReplaceEnabled{true};
    uint32_t LeftMargin{1}; // Margins are in glyphs.
    uint32_t DecorationMargin{1};
    uint32_t TextMargin{2};
    Palette Colors{DarkPalette()};
    const Language *Language = nullptr;
    LineBreakConfig LineBreak;
    AutoCompleteConfig AutoComplete;
    std::string FindButtonLabel{"Find"}, FindAllButtonLabel{"Find All"}, ReplaceButtonLabel{"Replace"}, ReplaceAllButtonLabel{"Replace All"};
};

// `Start` is the selection anchor and `End` is the caret. `End < Start` is a backward selection.
struct Cursor {
    DocPos Start, End;
    bool Main{false}, Current{true}, Updated{true};
    uint64_t PreferredColumn{0};

    DocPos SelectionStart() const { return Start < End ? Start : End; }
    DocPos SelectionEnd() const { return Start > End ? Start : End; }
    bool HasSelection() const { return Start != End; }
};

struct Cursors {
    std::vector<Cursor> Items;
    uint32_t Main{0}, Current{0};
};

// One insert or delete. Undo and redo replay these to shift folds, markers and squiggles.
struct Edit {
    bool Insert;
    DocPos Start, End;
};

struct Transaction {
    Text Before, After;
    Cursors BeforeCursors, AfterCursors;
    std::vector<Edit> Edits;
};

constexpr uint64_t InvalidLine = ~uint64_t{0};

// For an indentation-based language, also an invisible indentation block.
struct Bracket {
    uint64_t StartChar{0};
    DocPos Start;
    uint64_t EndChar{0};
    DocPos End;
    uint64_t Level{0};
    bool Visible{false};

    bool IsAfter(DocPos p) const { return Start > p; }
    bool IsAround(DocPos p) const { return Start < p && End >= p; }
    bool operator==(const Bracket &o) const { return Start == o.Start && End == o.End; }
};

enum struct FoldingState : uint8_t { Foldable,
                                     Folded,
                                     Visible,
                                     Hidden };

struct Fold {
    uint64_t Start, End;
};

struct Section {
    uint64_t StartIndex, EndIndex, Columns, Indent;
};

struct LineLayout {
    uint64_t Row{0}, Rows{1}, Columns{0};
    uint64_t FirstSection{~uint64_t{0}}; // Into `Typeset::Sections`, when the line wraps.
};

struct Row {
    uint64_t Line, Section, Columns;
};

struct MiniSection {
    uint64_t Start, End;
    Color Shade;
};
struct MiniRow {
    std::vector<MiniSection> Sections;
};

// The first fields are the cache key.
struct Typeset {
    const vec::Node *Root = nullptr, *Tail = nullptr;
    uint64_t Size{~uint64_t{0}};
    uint32_t TabSize{0};
    bool WordWrap{false};
    uint64_t WordWrapColumns{0};
    LineBreakConfig LineBreak;
    uint64_t FoldsVersion{~uint64_t{0}};
    std::vector<LineLayout> Lines;
    std::vector<Section> Sections;
    std::vector<Row> Rows;
    uint64_t TotalRows{0}, TotalColumns{0};
    uint64_t Version{0}; // Bumped on every rebuild, for caches over the layout.
};

// The first fields are the cache key.
struct View {
    const vec::Node *Root = nullptr, *Tail = nullptr;
    uint64_t Size{~uint64_t{0}};
    const Language *Lang = nullptr;
    bool ShowMatchingBrackets{false};
    uint32_t TabSize{0};
    std::vector<uint8_t> Colors; // One `Color` per glyph of the document.
    std::vector<uint8_t> LineStates; // The colorizer's state entering each line.
    std::vector<Bracket> Brackets;
    uint64_t Version{0};
};

struct Marker {
    ImU32 LineNumberColor, TextColor;
    std::string LineNumberTooltip, TextTooltip;
};

enum struct SquiggleStyle : uint8_t { Wave,
                                      Background };

struct Squiggle {
    uint64_t Type;
    ImU32 Shade;
    std::string Tooltip;
    SquiggleStyle Style;
};

struct AutoCompleteState {
    bool Active{false}, RequestActivation{false}, RequestDeactivation{false}, TriggeredManually{false};
    DocPos CurrentLocation, StartLocation;
    int64_t ActivationTimeUs{0};
    AutoCompleteContext Context;
    std::vector<std::string> Suggestions;
    size_t CurrentSelection{0};
};

struct State {
    Text Doc; // Codepoints, newlines included.
    Cursors Cursors;
    Settings Config;
    std::vector<Transaction> History;
    size_t UndoIndex{0};
    View Derived;
    Typeset Layout;
    std::vector<MiniRow> MiniRows;
    uint64_t MiniRowsLayout{~uint64_t{0}}, MiniRowsColors{~uint64_t{0}};
    bool MiniRowsFor{false};

    // `Folded` is the lines the user folded. `Folds`, `LineFolds` are derived from the brackets and `Folded`.
    std::vector<uint64_t> Folded;
    std::vector<Fold> Folds;
    std::vector<FoldingState> LineFolds;
    bool FoldsDirty{false}, LineFoldsFor{false};
    uint64_t FoldsVersion{0};

    // `Scroll` is the child window's scroll at this frame's Begin. `ScrollTarget` is a request for the next frame. FLT_MAX means none.
    ImVec2 Scroll{}, ScrollTarget{FLT_MAX, FLT_MAX};
    DocPos ScrollToDocPos{InvalidLine, 0};
    uint64_t ScrollToLineNumber{InvalidLine};
    enum struct Align : uint8_t { Top,
                                  Middle,
                                  Bottom } ScrollToAlignment{Align::Middle};
    float ScrollToFraction{0};
    bool ResetScroll{false};
    ImVec2 ScrollSeen{}; // `Scroll` as last read, to tell a restored state from a scrolled window.
    uint64_t FirstVisibleRow{0}, LastVisibleRow{0}, FirstVisibleColumn{0}, LastVisibleColumn{0};

    Palette Shaded{}, MiniShaded{}; // `Config.Colors` with the style alpha and the minimap alpha applied. Zero until the first frame applies them, as upstream's are.
    float ShadedAlpha{-1};
    uint64_t FirstMiniMapRow{0}, LastMiniMapRow{0};
    bool MiniMapIsScrollbar{false};
    float MiniMapScrollStart{0}, MiniMapScrollY{0};
    ImVec2 TotalSize{}; // Content size from the previous frame.
    bool FirstFrame{true}, FocusOnEditor{false};
    float CaretTimer{-0.3f};
    float CursorWidth{0}; // Set after the content size is computed, so the first frame's content size does not include it.
    bool DocUpdated{false};
    uint32_t TypesetTabSize{0}; // Settings the layout was last built with.
    bool TypesetWordWrap{false};
    uint64_t TypesetWordWrapColumns{0}, WordWrapColumns{0};

    // `LineMarkers[line]` and `GlyphSquiggles[offset]` are indices plus one into `Markers` and `Squiggles`, zero for none.
    std::vector<Marker> Markers;
    std::vector<uint64_t> LineMarkers;
    std::vector<Squiggle> Squiggles;
    vec::FlexVector GlyphSquiggles;

    // Microseconds, summed from each frame's delta time.
    int64_t ClockUs{0};
    AutoCompleteState AutoComplete;

    bool FindReplaceVisible{false}, FocusOnFind{false}, FindCancelledAutocomplete{false};
    std::string FindText, ReplaceText;
    bool CaseSensitiveFind{false}, WholeWordFind{false};

    bool NavigatingVertically{false};
    float LastClickTime{-1};
    uint32_t CompletePairCloser{0};
    DocPos CompletePairLocation;
    bool Panning{false}, Scrolling{false}, SelectingText{false}, DeletesHappened{false};
    ImVec2 ScrollStart{};

    // Set by `Update`, carried out by `Frame`.
    std::string ClipboardOut;
    bool SetClipboard{false};
};

State Initial(std::string_view text, Settings config = {});

// Lines and positions out of range are clamped.
void AddMarker(State &s, uint64_t line, ImU32 lineNumberColor, ImU32 textColor, std::string_view lineNumberTooltip, std::string_view textTooltip);
void ClearMarkers(State &s);
void AddSquiggle(State &s, DocPos start, DocPos end, uint64_t type, ImU32 color, std::string_view tooltip = {}, SquiggleStyle style = SquiggleStyle::Wave);
void ClearSquiggles(State &s, DocPos start, DocPos end);
void ClearSquiggles(State &s, uint64_t type);
void ClearSquiggles(State &s);

State Frame(State state, const char *title, ImVec2 size = {}, ImGuiChildFlags childFlags = 0, ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_HorizontalScrollbar);
} // namespace ipte
