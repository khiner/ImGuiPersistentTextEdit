#include "Update.h"

#include "Colorize.h"
#include "Cursors.h"
#include "Document.h"
#include "Layout.h"

#include <algorithm>

#include <limits>

namespace ipte {
void UnfoldAroundLine(State &s, uint64_t line) {
    for (const auto &fold : s.Folds) {
        if (line > fold.Start && line <= fold.End && FoldAt(s, fold.Start) == FoldingState::Folded) {
            std::erase(s.Folded, fold.Start);
            s.FoldsDirty = true;
        }
    }
}

void MakeCursorVisible(State &s) {
    s.ScrollToDocPos = CurrentCursor(s.Cursors).End;
    s.ScrollToLineNumber = InvalidLine;
    if (s.Config.LineFolding) UnfoldAroundLine(s, s.ScrollToDocPos.Line);
    s.CaretTimer = -0.3f;
}

namespace {
void ShiftFoldsForInsert(State &s, DocPos start, DocPos end) {
    const auto lines = end.Line - start.Line;
    if (!lines) return;
    for (auto &f : s.Folded)
        if (f > start.Line) f += lines;
}

vec::FlexVector Zeros(uint64_t count) {
    const std::vector<uint64_t> zeros(count, 0);
    return {zeros.begin(), zeros.end()};
}

void ShiftMarksForInsert(State &s, DocPos start, DocPos end) {
    const auto lines = end.Line - start.Line;
    if (lines && !s.LineMarkers.empty()) s.LineMarkers.insert(s.LineMarkers.begin() + int64_t(start.Line + 1), lines, 0);
    const auto from = Offset(s.Doc, start), count = Offset(s.Doc, end) - from;
    if (count && s.GlyphSquiggles.Size) s.GlyphSquiggles = vec::Insert(s.GlyphSquiggles, from, Zeros(count));
}

void ShiftMarksForDelete(State &s, DocPos start, DocPos end, const Text &before) {
    const auto lines = end.Line - start.Line;
    if (!s.LineMarkers.empty()) {
        if (lines) s.LineMarkers.erase(s.LineMarkers.begin() + int64_t(start.Line + 1), s.LineMarkers.begin() + int64_t(end.Line + 1));
        s.LineMarkers[start.Line] = 0;
    }
    if (s.GlyphSquiggles.Size) s.GlyphSquiggles = vec::Erase(s.GlyphSquiggles, Offset(before, start), Offset(before, end));
}

void ShiftFoldsForDelete(State &s, DocPos start, DocPos end) {
    const auto lines = end.Line - start.Line;
    if (!lines) return;
    std::erase_if(s.Folded, [&](uint64_t f) { return f > start.Line && f <= end.Line; });
    for (auto &f : s.Folded)
        if (f > end.Line) f -= lines;
}

void CancelAutoComplete(State &s) {
    if (s.AutoComplete.Active) s.AutoComplete.RequestDeactivation = true;
}

void StartAutoComplete(State &s) {
    auto &ac = s.AutoComplete;
    ac.RequestActivation = true;
    ac.CurrentLocation = MainCursor(s.Cursors).SelectionEnd();
    ac.ActivationTimeUs = s.ClockUs + s.Config.AutoComplete.TriggerDelayMs * 1000;
}

bool StartAutoCompleteTyping(State &s) {
    const auto &c = s.Config.AutoComplete;
    auto &ac = s.AutoComplete;
    if (ac.Active || ac.RequestActivation || !c.Enabled || !c.TriggerOnTyping) return false;
    ac.TriggeredManually = false;
    StartAutoComplete(s);
    return true;
}

bool StartAutoCompleteShortcut(State &s) {
    const auto &c = s.Config.AutoComplete;
    auto &ac = s.AutoComplete;
    if (ac.Active || ac.RequestActivation || !c.Enabled || !c.TriggerOnShortcut) return false;
    ac.TriggeredManually = true;
    StartAutoComplete(s);
    return true;
}

struct Tx {
    Text Before;
    Cursors BeforeCursors;
    std::vector<Edit> Edits;
};

Tx StartTransaction(State &s, bool cancelsAutoComplete = true) {
    if (cancelsAutoComplete) CancelAutoComplete(s);
    return {s.Doc, s.Cursors};
}

bool EndTransaction(State &s, Tx &tx) {
    if (tx.Edits.empty()) return false;
    UpdateCursors(s);
    s.History.resize(s.UndoIndex);
    s.History.push_back({tx.Before, s.Doc, tx.BeforeCursors, s.Cursors, std::move(tx.Edits)});
    ++s.UndoIndex;
    return true;
}

DocPos InsertText(State &s, Tx &tx, DocPos start, std::string_view text) {
    auto [doc, end] = InsertText(s.Config, s.Doc, start, text);
    s.Doc = std::move(doc);
    s.DocUpdated = true;
    ShiftFoldsForInsert(s, start, end);
    ShiftMarksForInsert(s, start, end);
    tx.Edits.push_back({true, start, end});
    MakeCursorVisible(s);
    return end;
}

void DeleteText(State &s, Tx &tx, DocPos start, DocPos end) {
    const auto before = s.Doc;
    s.Doc = DeleteText(s.Doc, start, end);
    s.DocUpdated = true;
    ShiftFoldsForDelete(s, start, end);
    ShiftMarksForDelete(s, start, end, before);
    tx.Edits.push_back({false, start, end});
    MakeCursorVisible(s);
    s.CaretTimer = -0.3f;
    s.DeletesHappened = true;
}

void InsertTextIntoAllCursors(State &s, Tx &tx, std::string_view text);

void DeleteTextFromAllCursors(State &s, Tx &tx) {
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].HasSelection()) continue;
        const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
        DeleteText(s, tx, start, end);
        Update(items[i], start, false);
        AdjustForDelete(s.Cursors, i, start, end);
    }
}

void InsertTextIntoAllCursors(State &s, Tx &tx, std::string_view text) {
    DeleteTextFromAllCursors(s, tx);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart();
        const auto end = InsertText(s, tx, start, text);
        Update(items[i], end, false);
        AdjustForInsert(s.Cursors, i, start, end);
    }
}

void AutoIndentAllCursors(State &s, Tx &tx) {
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart();
        if (items[i].HasSelection()) {
            const auto end = items[i].SelectionEnd();
            DeleteText(s, tx, start, end);
            AdjustForDelete(s.Cursors, i, start, end);
        }
        auto index = start.Index;
        const auto lineStart = LineStart(s.Doc, start.Line), lineSize = LineLength(s.Doc, start.Line);
        const auto at = [&](uint64_t k) { return s.Doc[lineStart + k]; };
        const uint64_t previousChar = index > 0 ? at(index - 1) : 0;
        const uint64_t nextChar = index < lineSize ? at(index) : 0;
        if (IsWhiteSpace(nextChar)) {
            while (index < lineSize && IsWhiteSpace(at(index))) ++index;
            const DocPos end{start.Line, index};
            DeleteText(s, tx, start, end);
            AdjustForDelete(s.Cursors, i, start, end);
        }
        std::string whitespace;
        for (uint64_t k = 0; k < LineLength(s.Doc, start.Line) && IsWhiteSpace(at(k)); ++k) whitespace += Utf8(at(k));
        std::string insert = "\n" + whitespace;
        auto newCursorIndex = whitespace.size();
        if (previousChar == '{' || previousChar == '[') {
            insert += '\t';
            ++newCursorIndex;
            if ((previousChar == '{' && nextChar == '}') || (previousChar == '[' && nextChar == ']')) insert += "\n" + whitespace;
        }
        const auto end = InsertText(s, tx, start, insert);
        AdjustForInsert(s.Cursors, i, start, end);
        Update(items[i], DocPos{start.Line + 1, newCursorIndex}, false);
    }
}

void MoveTo(State &s, DocPos pos, bool select) {
    ClearAdditional(s.Cursors);
    Update(CurrentCursor(s.Cursors), pos, select);
    MakeCursorVisible(s);
    s.NavigatingVertically = false;
}

void MoveVertically(State &s, uint64_t rows, bool select, bool up) {
    for (auto &c : s.Cursors.Items) {
        auto vis = DocToVis(s, c.End);
        if (s.NavigatingVertically) vis.Column = c.PreferredColumn;
        else c.PreferredColumn = vis.Column;
        if (up) vis.Row = rows > vis.Row ? 0 : vis.Row - rows;
        else vis.Row += rows;
        vis = NormalizeVis(s, vis);
        Update(c, VisToDoc(s, vis), select);
    }
    MakeCursorVisible(s);
    s.NavigatingVertically = true;
}

void MoveHorizontally(State &s, bool select, bool wordMode, bool left) {
    for (auto &c : s.Cursors.Items) {
        if (c.HasSelection() && !select && !wordMode) {
            if (left) ResetToStart(c);
            else ResetToEnd(c);
        } else {
            Update(c, left ? Left(s, c.End, wordMode) : Right(s, c.End, wordMode), select);
        }
    }
    MakeCursorVisible(s);
    s.NavigatingVertically = false;
}

void HandleCharacter(State &s, uint64_t character) {
    auto tx = StartTransaction(s, false);
    const auto opener = character;
    const auto isPaired = !s.Config.Overwrite && s.Config.CompletePairedGlyphs && IsPairOpener(opener);
    const auto closer = ToPairCloser(opener);
    auto &items = s.Cursors.Items;

    if (s.CompletePairCloser) {
        if (s.CompletePairCloser == character && s.CompletePairLocation == CurrentCursor(s.Cursors).SelectionEnd()) {
            s.CompletePairCloser = 0;
            MoveHorizontally(s, false, false, false);
            return;
        }
        s.CompletePairCloser = 0;
    }

    if (AnyHasSelection(s.Cursors) && isPaired) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (!items[i].HasSelection()) continue;
            const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
            const auto end1 = InsertText(s, tx, end, Utf8(closer));
            AdjustForInsert(s.Cursors, i, start, end1);
            const auto end2 = InsertText(s, tx, start, Utf8(opener));
            AdjustForInsert(s.Cursors, i, start, end2);
            Update(items[i], Right(s, start), Right(s, end));
        }
    } else if (isPaired) {
        const auto pair = Utf8(opener) + Utf8(closer);
        for (size_t i = 0; i < items.size(); ++i) {
            const auto start = items[i].SelectionStart();
            const auto end = InsertText(s, tx, start, pair);
            AdjustForInsert(s.Cursors, i, start, end);
            Update(items[i], Right(s, start), false);
        }
        s.CompletePairCloser = uint32_t(closer);
        s.CompletePairLocation = CurrentCursor(s.Cursors).SelectionEnd();
    } else if (!s.Config.Overwrite && s.Config.AutoIndent && character == '\n') {
        AutoIndentAllCursors(s, tx);
    } else {
        if (s.Config.Overwrite) {
            for (size_t i = 0; i < items.size(); ++i) {
                if (items[i].HasSelection()) continue;
                const auto start = items[i].SelectionStart();
                if (start != EndOfLine(s.Doc, start)) {
                    const auto end = Right(s, start);
                    DeleteText(s, tx, start, end);
                    AdjustForDelete(s.Cursors, i, start, end);
                }
            }
        }
        InsertTextIntoAllCursors(s, tx, Utf8(character));
    }
    EndTransaction(s, tx);
    if (IsWord(character) && !HasMultiple(s.Cursors) && StartAutoCompleteTyping(s)) MakeCursorVisible(s);
}

void HandleBackspace(State &s, bool wordMode) {
    auto tx = StartTransaction(s, false);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].HasSelection() ? items[i].SelectionStart() : Left(s, items[i].SelectionStart(), wordMode);
        const auto end = items[i].SelectionEnd();
        DeleteText(s, tx, start, end);
        Update(items[i], start, false);
        AdjustForDelete(s.Cursors, i, start, end);
    }
    EndTransaction(s, tx);
}

void HandleDelete(State &s, bool wordMode) {
    auto tx = StartTransaction(s, false);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart();
        const auto end = items[i].HasSelection() ? items[i].SelectionEnd() : Right(s, items[i].SelectionEnd(), wordMode);
        DeleteText(s, tx, start, end);
        Update(items[i], start, false);
        AdjustForDelete(s.Cursors, i, start, end);
    }
    EndTransaction(s, tx);
}

void RemoveSelectedLines(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = StartOfLine(items[i].SelectionStart());
        auto end = items[i].SelectionEnd();
        end = end.Index == 0 ? end : NextLine(s, end);
        DeleteText(s, tx, start, end);
        Update(items[i], start, false);
        AdjustForDelete(s.Cursors, i, start, end);
    }
    EndTransaction(s, tx);
}

void InsertLineAbove(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = StartOfLine(items[i].SelectionStart());
        const auto end = InsertText(s, tx, start, "\n");
        Update(items[i], start, false);
        AdjustForInsert(s.Cursors, i, start, end);
    }
    EndTransaction(s, tx);
}

void InsertLineBelow(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        auto start = items[i].SelectionEnd();
        start = start.Index == 0 ? start : NextLine(s, start);
        const auto end = InsertText(s, tx, start, "\n");
        Update(items[i], start, false);
        AdjustForInsert(s.Cursors, i, start, end);
    }
    EndTransaction(s, tx);
}

void IndentLines(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto cursorStart = items[i].SelectionStart(), cursorEnd = items[i].SelectionEnd();
        for (auto line = cursorStart.Line; line <= cursorEnd.Line; ++line) {
            if ((!items[i].HasSelection() || DocPos{line, 0} != cursorEnd) && LineLength(s.Doc, line)) {
                const DocPos insertStart{line, 0};
                const auto insertEnd = InsertText(s, tx, insertStart, "\t");
                AdjustForInsert(s.Cursors, i, insertStart, insertEnd, true);
            }
        }
    }
    EndTransaction(s, tx);
}

void DeindentLines(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto cursorStart = items[i].SelectionStart(), cursorEnd = items[i].SelectionEnd();
        for (auto line = cursorStart.Line; line <= cursorEnd.Line; ++line) {
            if ((!items[i].HasSelection() || DocPos{line, 0} != cursorEnd) && LineLength(s.Doc, line)) {
                uint64_t column = 0, index = 0;
                const auto lineStart = LineStart(s.Doc, line), size = LineLength(s.Doc, line);
                while (column < s.Config.TabSize && index < size && (s.Doc[lineStart + index] == ' ' || s.Doc[lineStart + index] == '\t')) {
                    column += Columns(s.Config, s.Doc[lineStart + index], column);
                    ++index;
                }
                const DocPos deleteStart{line, 0}, deleteEnd{line, index};
                if (deleteEnd != deleteStart) {
                    DeleteText(s, tx, deleteStart, deleteEnd);
                    AdjustForDelete(s.Cursors, i, deleteStart, deleteEnd, true);
                }
            }
        }
    }
    EndTransaction(s, tx);
}

void MoveUpLines(State &s) {
    if (s.Cursors.Items[0].SelectionStart().Line == 0) return;
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
        const auto deleteStart = StartOfLine(start);
        const auto deleteEnd = end.Index == 0 ? end : NextLine(s, end);
        const auto text = SectionText(s.Doc, deleteStart, deleteEnd);
        DeleteText(s, tx, deleteStart, deleteEnd);
        AdjustForDelete(s.Cursors, i, deleteStart, deleteEnd);
        const DocPos insertStart{deleteStart.Line - 1, 0};
        const auto insertEnd = InsertText(s, tx, insertStart, text);
        AdjustForInsert(s.Cursors, i, insertStart, insertEnd);
        Update(items[i], {start.Line - 1, start.Index}, {end.Line - 1, end.Index});
    }
    EndTransaction(s, tx);
}

void MoveDownLines(State &s) {
    if (IsLastLine(s.Doc, s.Cursors.Items.back().SelectionStart().Line)) return;
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
        const auto deleteStart = StartOfLine(start);
        const auto deleteEnd = end.Index == 0 ? end : NextLine(s, end);
        const auto text = SectionText(s.Doc, deleteStart, deleteEnd);
        DeleteText(s, tx, deleteStart, deleteEnd);
        AdjustForDelete(s.Cursors, i, deleteStart, deleteEnd);
        const auto insertStart = NextLine(s, deleteStart);
        const auto insertEnd = InsertText(s, tx, insertStart, text);
        AdjustForInsert(s.Cursors, i, insertStart, insertEnd);
        Update(items[i], {start.Line + 1, start.Index}, {end.Line + 1, end.Index});
    }
    EndTransaction(s, tx);
}

void Copy(State &s) {
    std::string text;
    if (AnyHasSelection(s.Cursors)) {
        for (const auto &c : s.Cursors.Items) {
            if (!text.empty()) text += '\n';
            if (c.HasSelection()) text += SectionText(s.Doc, c.SelectionStart(), c.SelectionEnd());
            else text += LineText(s.Doc, c.SelectionStart().Line);
        }
    } else {
        for (const auto &c : s.Cursors.Items) text += LineText(s.Doc, c.SelectionStart().Line) + "\n";
    }
    s.ClipboardOut = std::move(text);
    s.SetClipboard = true;
}

void Cut(State &s) {
    Copy(s);
    auto tx = StartTransaction(s);
    DeleteTextFromAllCursors(s, tx);
    ResetToStart(CurrentCursor(s.Cursors));
    EndTransaction(s, tx);
}

void Paste(State &s, std::string_view clipboard) {
    auto tx = StartTransaction(s);
    InsertTextIntoAllCursors(s, tx, clipboard);
    EndTransaction(s, tx);
}

void Undo(State &s) {
    if (s.UndoIndex == 0) return;
    const auto &t = s.History[--s.UndoIndex];
    // Replay the edits backwards on the folds, markers and squiggles.
    auto doc = s.Doc;
    for (auto e = t.Edits.rbegin(); e != t.Edits.rend(); ++e) {
        if (e->Insert) {
            ShiftFoldsForDelete(s, e->Start, e->End);
            ShiftMarksForDelete(s, e->Start, e->End, doc);
            doc = DeleteText(doc, e->Start, e->End);
        } else {
            ShiftFoldsForInsert(s, e->Start, e->End);
            const auto count = Offset(t.Before, e->End) - Offset(t.Before, e->Start);
            doc = vec::Insert(doc, Offset(doc, e->Start), vec::Take(vec::Drop(t.Before, Offset(t.Before, e->Start)), count));
            ShiftMarksForInsert(s, e->Start, e->End);
        }
    }
    s.Doc = t.Before;
    s.Cursors = t.BeforeCursors;
    s.DocUpdated = true;
    s.DeletesHappened = true;
    MakeCursorVisible(s);
}

void Redo(State &s) {
    if (s.UndoIndex >= s.History.size()) return;
    const auto &t = s.History[s.UndoIndex++];
    auto doc = s.Doc;
    for (const auto &e : t.Edits) {
        if (e.Insert) {
            ShiftFoldsForInsert(s, e.Start, e.End);
            const auto count = Offset(t.After, e.End) - Offset(t.After, e.Start);
            doc = vec::Insert(doc, Offset(doc, e.Start), vec::Take(vec::Drop(t.After, Offset(t.After, e.Start)), count));
            ShiftMarksForInsert(s, e.Start, e.End);
        } else {
            ShiftFoldsForDelete(s, e.Start, e.End);
            ShiftMarksForDelete(s, e.Start, e.End, doc);
            doc = DeleteText(doc, e.Start, e.End);
        }
    }
    s.Doc = t.After;
    s.Cursors = t.AfterCursors;
    s.DocUpdated = true;
    s.DeletesHappened = true;
    MakeCursorVisible(s);
}

void ToggleFoldAtLine(State &s, uint64_t line) {
    const auto state = FoldAt(s, line);
    if (state == FoldingState::Foldable) {
        s.Folded.push_back(line);
        std::ranges::sort(s.Folded);
        s.FoldsDirty = true;
    } else if (state == FoldingState::Folded) {
        std::erase(s.Folded, line);
        for (const auto &fold : s.Folds)
            if (fold.Start == line) std::erase_if(s.Folded, [&](uint64_t f) { return f > fold.Start && f <= fold.End; });
        s.FoldsDirty = true;
    }
}

void GrowSelections(State &s) {
    for (auto &c : s.Cursors.Items) {
        if (c.HasSelection()) {
            const auto start = c.SelectionStart(), end = c.SelectionEnd();
            const auto startCp = CodepointAt(s.Doc, Left(s, start)), endCp = CodepointAt(s.Doc, end);
            if (IsBracketOpener(startCp) && endCp == ToPairCloser(startCp)) {
                Update(c, Left(s, start), Right(s, end));
            } else if (const auto *bracket = EnclosingBrackets(s.Derived, start, end)) {
                Update(c, Right(s, bracket->Start), bracket->End);
            }
        } else {
            const auto pos = c.SelectionEnd();
            Update(c, FindWordStart(s.Doc, pos), FindWordEnd(s.Doc, pos));
        }
    }
}

void ShrinkSelections(State &s) {
    for (auto &c : s.Cursors.Items) {
        if (!c.HasSelection()) continue;
        const auto start = c.SelectionStart(), end = c.SelectionEnd();
        const auto startCp = CodepointAt(s.Doc, start), endCp = CodepointAt(s.Doc, Left(s, end));
        if (IsBracketOpener(startCp) && endCp == ToPairCloser(startCp)) {
            Update(c, Right(s, start), Left(s, end));
        } else if (const auto *bracket = InnerBrackets(s.Derived, start, end)) {
            Update(c, bracket->Start, Right(s, bracket->End));
        }
    }
}

void SetScrollTarget(State &s, ImVec2 target) { s.ScrollTarget = target; }

void SelectFirstOccurrenceOf(State &s, std::string_view text, bool caseSensitive, bool wholeWord) {
    DocPos start, end;
    if (FindText(s.Doc, {}, text, caseSensitive, wholeWord, start, end)) {
        SetCursor(s.Cursors, start, end);
        MakeCursorVisible(s);
    } else {
        ClearAdditional(s.Cursors, true);
    }
}

void SelectNextOccurrenceOf(State &s, std::string_view text, bool caseSensitive, bool wholeWord) {
    DocPos start, end;
    if (FindText(s.Doc, CurrentCursor(s.Cursors).SelectionEnd(), text, caseSensitive, wholeWord, start, end)) {
        SetCursor(s.Cursors, start, end);
        MakeCursorVisible(s);
    } else {
        ClearAdditional(s.Cursors, true);
    }
}

void SelectAllOccurrencesOf(State &s, std::string_view text, bool caseSensitive, bool wholeWord) {
    DocPos start, end;
    if (!FindText(s.Doc, {}, text, caseSensitive, wholeWord, start, end)) {
        ClearAdditional(s.Cursors, true);
        return;
    }
    SetCursor(s.Cursors, start, end);
    while (true) {
        DocPos nextStart, nextEnd;
        FindText(s.Doc, CurrentCursor(s.Cursors).SelectionEnd(), text, caseSensitive, wholeWord, nextStart, nextEnd);
        if (nextStart == start && nextEnd == end) break;
        AddCursor(s.Cursors, nextStart, nextEnd);
    }
}

void AddNextOccurrence(State &s, bool wholeWord) {
    const auto cursor = CurrentCursor(s.Cursors);
    const auto text = SectionText(s.Doc, cursor.SelectionStart(), cursor.SelectionEnd());
    DocPos start, end;
    if (FindText(s.Doc, cursor.SelectionEnd(), text, true, wholeWord, start, end)) AddCursor(s.Cursors, start, end);
}

void SelectAllOccurrences(State &s, bool wholeWord) {
    const auto cursor = CurrentCursor(s.Cursors);
    SelectAllOccurrencesOf(s, SectionText(s.Doc, cursor.SelectionStart(), cursor.SelectionEnd()), true, wholeWord);
}

void ReplaceTextInCurrentCursor(State &s, std::string_view text) {
    auto tx = StartTransaction(s);
    const auto i = s.Cursors.Current;
    const auto start = s.Cursors.Items[i].SelectionStart(), end = s.Cursors.Items[i].SelectionEnd();
    DeleteText(s, tx, start, end);
    AdjustForDelete(s.Cursors, i, start, end);
    const auto newEnd = InsertText(s, tx, start, text);
    Update(s.Cursors.Items[i], newEnd, false);
    AdjustForInsert(s.Cursors, i, start, newEnd);
    EndTransaction(s, tx);
}

void ReplaceTextInAllCursors(State &s, std::string_view text) {
    auto tx = StartTransaction(s);
    InsertTextIntoAllCursors(s, tx, text);
    EndTransaction(s, tx);
}

void ReplaceSectionText(State &s, DocPos start, DocPos end, std::string_view text) {
    auto tx = StartTransaction(s);
    DeleteText(s, tx, start, end);
    const auto newEnd = InsertText(s, tx, start, text);
    ClearAdditional(s.Cursors);
    Update(MainCursor(s.Cursors), newEnd, newEnd);
    EndTransaction(s, tx);
}

void OpenFindReplace(State &s) {
    if (s.AutoComplete.Active) {
        CancelAutoComplete(s);
        s.FindCancelledAutocomplete = true;
    }
    const auto cursor = MainCursor(s.Cursors);
    if (cursor.HasSelection()) {
        if (cursor.SelectionStart().Line == cursor.SelectionEnd().Line) s.FindText = SectionText(s.Doc, cursor.SelectionStart(), cursor.SelectionEnd());
    } else {
        const auto word = WholeWord(s.Doc, cursor.SelectionStart(), true);
        if (word.Start != word.End) s.FindText = SectionText(s.Doc, word.Start, word.End);
    }
    s.FindReplaceVisible = true;
    s.FocusOnFind = true;
}

void CloseFindReplace(State &s) {
    if (!s.FindReplaceVisible) return;
    s.FindReplaceVisible = false;
    s.FocusOnEditor = true;
    s.FocusOnFind = false;
}

void FocusEditor(State &s) {
    s.FocusOnEditor = true;
    s.FocusOnFind = false;
}

void Find(State &s) {
    if (s.FindText.empty()) return;
    SelectNextOccurrenceOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    FocusEditor(s);
}

void FindAll(State &s) {
    if (s.FindText.empty()) return;
    SelectAllOccurrencesOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    FocusEditor(s);
}

void Replace(State &s) {
    if (s.FindText.empty()) return;
    if (!AnyHasSelection(s.Cursors)) SelectNextOccurrenceOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    ReplaceTextInCurrentCursor(s, s.ReplaceText);
    SelectNextOccurrenceOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    FocusEditor(s);
}

void ReplaceAll(State &s) {
    if (s.FindText.empty()) return;
    SelectAllOccurrencesOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    ReplaceTextInAllCursors(s, s.ReplaceText);
    FocusEditor(s);
}
} // namespace

void UpdateAutoCompleteContext(State &s) {
    auto &ac = s.AutoComplete;
    auto &ctx = ac.Context;
    ctx.SearchTerm = SectionText(s.Doc, ac.StartLocation, ac.CurrentLocation);
    if (ac.CurrentLocation.Index == 0) {
        ctx.InIdentifier = ctx.InNumber = false;
        ctx.InComment = LineInComment(s, ac.CurrentLocation.Line);
        ctx.InString = LineInString(s, ac.CurrentLocation.Line);
    } else {
        const auto left = Left(s, ac.CurrentLocation);
        const auto color = Offset(s.Doc, left) < LineEnd(s.Doc, left.Line) ? ColorAt(s, Offset(s.Doc, left)) : Color::Text;
        ctx.InIdentifier = color == Color::Identifier || color == Color::KnownIdentifier;
        ctx.InNumber = color == Color::Number;
        ctx.InComment = color == Color::Comment;
        ctx.InString = color == Color::String;
    }
    ctx.Start = ac.StartLocation;
    ctx.End = ac.CurrentLocation;
    ctx.Lang = s.Config.Language;
}

void RefreshSuggestions(State &s) {
    auto &ac = s.AutoComplete;
    if (s.Config.AutoComplete.Provider) ac.Suggestions = s.Config.AutoComplete.Provider(ac.Context);
    else ac.Suggestions.clear();
    ac.CurrentSelection = 0;
}

void AddMarker(State &s, uint64_t line, ImU32 lineNumberColor, ImU32 textColor, std::string_view lineNumberTooltip, std::string_view textTooltip) {
    line = NormalizeLine(s.Doc, line);
    if (s.LineMarkers.size() != LineCount(s.Doc)) s.LineMarkers.assign(LineCount(s.Doc), 0);
    s.Markers.push_back({lineNumberColor, textColor, std::string(lineNumberTooltip), std::string(textTooltip)});
    s.LineMarkers[line] = s.Markers.size();
}

void ClearMarkers(State &s) {
    s.LineMarkers.clear();
    s.Markers.clear();
}

void AddSquiggle(State &s, DocPos start, DocPos end, uint64_t type, ImU32 color, std::string_view tooltip, SquiggleStyle style) {
    start = Normalize(s.Doc, start);
    end = Normalize(s.Doc, end);
    if (!(start < end)) return;
    if (s.GlyphSquiggles.Size != s.Doc.Size) s.GlyphSquiggles = Zeros(s.Doc.Size);
    s.Squiggles.push_back({type, color, std::string(tooltip), style});
    const auto index = s.Squiggles.size();
    auto t = s.GlyphSquiggles.transient();
    for (auto i = Offset(s.Doc, start), last = Offset(s.Doc, end); i < last; ++i) t.set(i, index);
    s.GlyphSquiggles = std::move(t).persistent();
    s.DeletesHappened = true; // Compress unused squiggles next frame.
}

void ClearSquiggles(State &s, DocPos start, DocPos end) {
    if (s.GlyphSquiggles.Size != s.Doc.Size) return;
    auto t = s.GlyphSquiggles.transient();
    for (auto i = Offset(s.Doc, Normalize(s.Doc, start)), last = Offset(s.Doc, Normalize(s.Doc, end)); i < last; ++i) t.set(i, 0);
    s.GlyphSquiggles = std::move(t).persistent();
    s.DeletesHappened = true;
}

void ClearSquiggles(State &s, uint64_t type) {
    if (s.GlyphSquiggles.Size != s.Doc.Size) return;
    auto t = s.GlyphSquiggles.transient();
    for (uint64_t i = 0; i < s.Doc.Size; ++i)
        if (const auto id = s.GlyphSquiggles[i]; id && s.Squiggles[id - 1].Type == type) t.set(i, 0);
    s.GlyphSquiggles = std::move(t).persistent();
    s.DeletesHappened = true;
}

void ClearSquiggles(State &s) {
    s.GlyphSquiggles = {};
    s.Squiggles.clear();
}

State Update(State s, const Action &a) {
    switch (a.Kind) {
        case Op::MoveUp: MoveVertically(s, a.Count, a.Select, true); break;
        case Op::MoveDown: MoveVertically(s, a.Count, a.Select, false); break;
        case Op::MoveLeft: MoveHorizontally(s, a.Select, a.Word, true); break;
        case Op::MoveRight: MoveHorizontally(s, a.Select, a.Word, false); break;
        case Op::MoveToTop: MoveTo(s, {}, a.Select); break;
        case Op::MoveToBottom: MoveTo(s, Bottom(s.Doc), a.Select); break;
        case Op::MoveToStartOfLine: MoveTo(s, StartOfLine(CurrentCursor(s.Cursors).End), a.Select); break;
        case Op::MoveToEndOfLine: MoveTo(s, EndOfLine(s.Doc, CurrentCursor(s.Cursors).End), a.Select); break;
        case Op::SelectAll:
            MoveTo(s, {}, false);
            MoveTo(s, Bottom(s.Doc), true);
            break;
        case Op::GrowSelections: GrowSelections(s); break;
        case Op::ShrinkSelections: ShrinkSelections(s); break;
        case Op::Cut: Cut(s); break;
        case Op::Copy: Copy(s); break;
        case Op::Paste: Paste(s, a.Text); break;
        case Op::Undo: Undo(s); break;
        case Op::Redo: Redo(s); break;
        case Op::Delete: HandleDelete(s, a.Word); break;
        case Op::Backspace: HandleBackspace(s, a.Word); break;
        case Op::RemoveSelectedLines: RemoveSelectedLines(s); break;
        case Op::DeindentLines: DeindentLines(s); break;
        case Op::IndentLines: IndentLines(s); break;
        case Op::MoveUpLines: MoveUpLines(s); break;
        case Op::MoveDownLines: MoveDownLines(s); break;
        case Op::ToggleOverwrite: s.Config.Overwrite = !s.Config.Overwrite; break;
        case Op::Character: HandleCharacter(s, a.Codepoint); break;
        case Op::InsertLineBelow: InsertLineBelow(s); break;
        case Op::InsertLineAbove: InsertLineAbove(s); break;
        case Op::Tab:
            if (AnyHasSelection(s.Cursors)) IndentLines(s);
            else HandleCharacter(s, '\t');
            break;
        case Op::ClearAdditionalCursors: ClearAdditional(s.Cursors); break;
        case Op::AddNextOccurrence: AddNextOccurrence(s, a.Word); break;
        case Op::SelectAllOccurrences: SelectAllOccurrences(s, a.Word); break;
        case Op::OpenFindReplace: OpenFindReplace(s); break;
        case Op::CloseFindReplace: CloseFindReplace(s); break;
        case Op::Find:
        case Op::FindNext: Find(s); break;
        case Op::FindAll: FindAll(s); break;
        case Op::Replace: Replace(s); break;
        case Op::ReplaceAll: ReplaceAll(s); break;
        case Op::SelectFirstOccurrence:
            s.FindText = a.Text;
            if (!s.FindText.empty()) SelectFirstOccurrenceOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
            else ClearAll(s.Cursors);
            break;
        case Op::FocusEditor: FocusEditor(s); break;
        case Op::ToggleCaseSensitiveFind:
            s.CaseSensitiveFind = !s.CaseSensitiveFind;
            Find(s);
            break;
        case Op::ToggleWholeWordFind:
            s.WholeWordFind = !s.WholeWordFind;
            Find(s);
            break;
        case Op::PanBy: SetScrollTarget(s, s.Scroll - a.Vec); break;
        case Op::ScrollModeBy: SetScrollTarget(s, s.Scroll - a.Vec); break;
        case Op::StopScrollMode: s.Scrolling = false; break;
        case Op::DragLineSelect: {
            auto &c = CurrentCursor(s.Cursors);
            const DocPos start{a.Pos.Line, 0};
            const auto end = Normalize(s.Doc, {a.Pos.Line + 1, 0});
            Update(c, c.End < c.Start ? start : end);
            MakeCursorVisible(s);
            break;
        }
        case Op::DragSelect:
            Update(CurrentCursor(s.Cursors), a.Pos);
            MakeCursorVisible(s);
            break;
        case Op::MouseReleased:
            s.MiniMapIsScrollbar = false;
            s.SelectingText = false;
            break;
        case Op::BeginPan: s.Panning = true; break;
        case Op::BeginScrollMode:
            s.Scrolling = true;
            s.ScrollStart = a.Vec;
            break;
        case Op::TripleClick: {
            const auto start = StartOfLine(a.Pos);
            const auto end = Normalize(s.Doc, {start.Line + 1, 0});
            Update(CurrentCursor(s.Cursors), start, end);
            break;
        }
        case Op::DoubleClick: {
            const auto cp = CodepointAt(s.Doc, a.Pos);
            bool handled = false;
            if (IsBracketOpener(cp)) {
                if (const auto *b = EnclosingBrackets(s.Derived, Right(s, a.Pos))) {
                    if (a.Select) SetCursor(s.Cursors, b->Start, Right(s, b->End));
                    else SetCursor(s.Cursors, Right(s, b->Start), b->End);
                    handled = true;
                }
            } else if (IsBracketCloser(cp)) {
                if (const auto *b = EnclosingBrackets(s.Derived, a.Pos)) {
                    SetCursor(s.Cursors, b->Start, Right(s, b->End));
                    handled = true;
                }
            }
            if (!handled && a.Pos.Index != LineLength(s.Doc, a.Pos.Line)) {
                const auto word = WholeWord(s.Doc, a.Pos);
                if (word.Start != word.End) Update(CurrentCursor(s.Cursors), word.Start, word.End);
            }
            break;
        }
        case Op::ClickLineNumbers: {
            const DocPos start{a.Pos.Line, 0};
            const auto end = Normalize(s.Doc, {a.Pos.Line + 1, 0});
            if (a.Select) {
                auto &c = CurrentCursor(s.Cursors);
                Update(c, c.End < c.Start ? start : end);
                CancelAutoComplete(s);
            } else if (a.Word) {
                AddCursor(s.Cursors, start, end);
                CancelAutoComplete(s);
            } else {
                SetCursor(s.Cursors, start, end);
            }
            MakeCursorVisible(s);
            break;
        }
        case Op::ClickText:
            if (a.Select) {
                Grow(CurrentCursor(s.Cursors), a.Pos);
                CancelAutoComplete(s);
            } else if (a.Word) {
                AddCursor(s.Cursors, a.Pos);
                CancelAutoComplete(s);
            } else {
                SetCursor(s.Cursors, a.Pos);
            }
            MakeCursorVisible(s);
            break;
        case Op::MouseDown:
            UpdateCursors(s);
            s.NavigatingVertically = false;
            break;
        case Op::ToggleFold: ToggleFoldAtLine(s, a.Pos.Line); break;
        case Op::MiniMapDrag: {
            const auto localOffset = a.Vec.y - s.MiniMapScrollStart;
            const auto documentOffset = localOffset * s.TotalSize.y / a.Vec.x;
            SetScrollTarget(s, {FLT_MAX, std::clamp(s.MiniMapScrollY + documentOffset, 0.0f, s.TotalSize.y - a.Vec.x)});
            break;
        }
        case Op::ClickMiniMap:
            if (a.Select) {
                s.MiniMapIsScrollbar = true;
                s.MiniMapScrollStart = a.Vec.y;
                s.MiniMapScrollY = s.Scroll.y;
            } else {
                s.ScrollToDocPos = {InvalidLine, 0};
                s.ScrollToLineNumber = NormalizeLine(s.Doc, a.Count);
                s.ScrollToAlignment = State::Align::Middle;
                s.ScrollToFraction = 0.0f;
                if (s.Config.LineFolding) UnfoldAroundLine(s, a.Count);
            }
            break;
        case Op::AutoCompleteShortcut:
            if (!HasMultiple(s.Cursors) && StartAutoCompleteShortcut(s)) MakeCursorVisible(s);
            break;
        case Op::AutoCompleteCancel: CancelAutoComplete(s); break;
        case Op::AutoCompleteAccept: {
            const auto start = s.AutoComplete.StartLocation;
            ReplaceSectionText(s, start, FindWordEnd(s.Doc, start, true), a.Text);
            break;
        }
    }
    return s;
}
} // namespace ipte
