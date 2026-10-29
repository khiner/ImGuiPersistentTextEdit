#pragma once

#include "TextEdit.h"

namespace ipte {
enum struct Op : uint8_t {
    MoveUp,
    MoveDown,
    MoveLeft,
    MoveRight,
    MoveToTop,
    MoveToBottom,
    MoveToStartOfLine,
    MoveToEndOfLine,
    SelectAll,
    GrowSelections,
    ShrinkSelections,
    Cut,
    Copy,
    Paste,
    Undo,
    Redo,
    Delete,
    Backspace,
    RemoveSelectedLines,
    DeindentLines,
    IndentLines,
    MoveUpLines,
    MoveDownLines,
    ToggleOverwrite,
    Character,
    InsertLineBelow,
    InsertLineAbove,
    Tab,
    ClearAdditionalCursors,
    AddNextOccurrence, // `Word`: whole words only.
    SelectAllOccurrences,
    OpenFindReplace,
    CloseFindReplace,
    Find,
    FindNext,
    FindAll,
    Replace,
    ReplaceAll,
    SelectFirstOccurrence, // `Text` is the new find text. Empty clears the cursors.
    FocusEditor,
    ToggleCaseSensitiveFind,
    ToggleWholeWordFind,
    PanBy, // `Vec`: drag delta.
    ScrollModeBy, // `Vec`: scroll offset.
    StopScrollMode,
    DragLineSelect, // `Pos`: caret position under the mouse.
    DragSelect,
    MouseReleased,
    BeginPan,
    BeginScrollMode, // `Vec`: mouse position.
    TripleClick,
    DoubleClick, // `Pos`: glyph under the mouse.
    ClickLineNumbers, // `Select`: extend the cursor. `Word`: add a cursor.
    ClickText,
    MouseDown, // Any button. Merges cursors and ends vertical navigation.
    ToggleFold,
    MiniMapDrag, // `Vec.x`: text height. `Vec.y`: mouse y within the window.
    ClickMiniMap, // `Count`: line clicked. `Select`: the row is visible, so start dragging.
    AutoCompleteShortcut,
    AutoCompleteCancel,
    AutoCompleteAccept, // `Text`: the replacement.
};

struct Action {
    Op Kind;
    bool Select{false}, Word{false};
    uint64_t Count{0};
    uint64_t Codepoint{0};
    DocPos Pos;
    std::string Text;
    ImVec2 Vec{};
};

State Update(State s, const Action &a);
void MakeCursorVisible(State &s);
void RefreshSuggestions(State &s);
void UpdateAutoCompleteContext(State &s);
void UnfoldAroundLine(State &s, uint64_t line);
} // namespace ipte
