#include "Update.h"

#include "Colorize.h"
#include "Cursors.h"
#include "Document.h"
#include "Layout.h"

#include <algorithm>
#include <ranges>

namespace ipte {
void UnfoldAroundLine(State &s, uint64_t line) {
    for (const auto &fold : s.Folds) {
        if (line > fold.Start && line <= fold.End && FoldAt(s, fold.Start) == FoldingState::Folded) {
            std::erase(s.Folded, fold.Start);
            s.FoldsDirty = true;
        }
    }
}

void ScrollToLine(State &s, uint64_t line, State::Align alignment, float fraction) {
    s.ScrollToDocPos = {InvalidLine, 0};
    s.ScrollToLineNumber = NormalizeLine(s.Doc, line);
    s.ScrollToAlignment = alignment;
    s.ScrollToFraction = fraction;
    if (s.Config.LineFolding) UnfoldAroundLine(s, line);
}

namespace {
void MakeCursorVisible(State &s) {
    s.ScrollToDocPos = CurrentCursor(s.Cursors).End;
    s.ScrollToLineNumber = InvalidLine;
    if (s.Config.LineFolding) UnfoldAroundLine(s, s.ScrollToDocPos.Line);
    s.CaretTimer = -0.3f;
}

vec::FlexVector Zeros(uint64_t count) {
    const std::vector<uint64_t> zeros(count, 0);
    return {zeros.begin(), zeros.end()};
}

void ShiftForInsert(State &s, DocPos start, DocPos end) {
    if (const auto lines = end.Line - start.Line) {
        for (auto &f : s.Folded)
            if (f > start.Line) f += lines;
        if (!s.LineMarkers.empty()) s.LineMarkers.insert(s.LineMarkers.begin() + int64_t(start.Line + 1), lines, 0);
    }
    const auto from = Offset(s.Doc, start), count = Offset(s.Doc, end) - from;
    if (count && s.GlyphSquiggles.Size) s.GlyphSquiggles = vec::Insert(s.GlyphSquiggles, from, Zeros(count));
}

void ShiftForDelete(State &s, DocPos start, DocPos end, const Text &before) {
    if (const auto lines = end.Line - start.Line) {
        std::erase_if(s.Folded, [&](uint64_t f) { return f > start.Line && f <= end.Line; });
        for (auto &f : s.Folded)
            if (f > end.Line) f -= lines;
        if (!s.LineMarkers.empty()) s.LineMarkers.erase(s.LineMarkers.begin() + int64_t(start.Line + 1), s.LineMarkers.begin() + int64_t(end.Line + 1));
    }
    if (!s.LineMarkers.empty()) s.LineMarkers[start.Line] = 0;
    if (s.GlyphSquiggles.Size) s.GlyphSquiggles = vec::Erase(s.GlyphSquiggles, Offset(before, start), Offset(before, end));
}

void CancelAutoComplete(State &s) {
    if (s.AutoComplete.Active) s.AutoComplete.RequestDeactivation = true;
}

void StartAutoComplete(State &s, bool manually) {
    const auto &c = s.Config.AutoComplete;
    auto &ac = s.AutoComplete;
    if (HasMultiple(s.Cursors) || ac.Active || ac.RequestActivation || !c.Enabled || !(manually ? c.TriggerOnShortcut : c.TriggerOnTyping)) return;
    ac.RequestActivation = true;
    ac.TriggeredManually = manually;
    ac.CurrentLocation = MainCursor(s.Cursors).SelectionEnd();
    ac.ActivationTimeUs = s.ClockUs + c.TriggerDelayMs * 1000;
    MakeCursorVisible(s);
}

Transaction StartTransaction(State &s, bool cancelsAutoComplete = true) {
    if (cancelsAutoComplete) CancelAutoComplete(s);
    return {.Before = s.Doc, .BeforeCursors = s.Cursors};
}

void EndTransaction(State &s, Transaction &tx) {
    if (tx.Edits.empty()) return;
    UpdateCursors(s);
    tx.After = s.Doc;
    tx.AfterCursors = s.Cursors;
    s.History.resize(s.UndoIndex);
    s.History.push_back(std::move(tx));
    ++s.UndoIndex;
}

DocPos InsertText(State &s, Transaction &tx, DocPos start, std::string_view text) {
    auto [doc, end] = InsertText(s.Config, s.Doc, start, text);
    s.Doc = std::move(doc);
    s.DocUpdated = true;
    ShiftForInsert(s, start, end);
    tx.Edits.push_back({true, start, end});
    MakeCursorVisible(s);
    return end;
}

void DeleteText(State &s, Transaction &tx, DocPos start, DocPos end) {
    ShiftForDelete(s, start, end, s.Doc);
    s.Doc = DeleteText(s.Doc, start, end);
    s.DocUpdated = true;
    s.DeletesHappened = true;
    tx.Edits.push_back({false, start, end});
    MakeCursorVisible(s);
}

// Inserts `text` for cursor `i` and shifts the cursors after it, or from it with `includeCurrent`.
DocPos InsertForCursor(State &s, Transaction &tx, size_t i, DocPos start, std::string_view text, bool includeCurrent = false) {
    const auto end = InsertText(s, tx, start, text);
    AdjustForInsert(s.Cursors, i, start, end, includeCurrent);
    return end;
}

// Deletes `start` to `end` for cursor `i` and shifts the cursors after it, or from it with `includeCurrent`.
void DeleteForCursor(State &s, Transaction &tx, size_t i, DocPos start, DocPos end, bool includeCurrent = false) {
    DeleteText(s, tx, start, end);
    AdjustForDelete(s.Cursors, i, start, end, includeCurrent);
}

std::string SelectedText(const Text &doc, const Cursor &c) { return SectionText(doc, c.SelectionStart(), c.SelectionEnd()); }

// Returns the selection end if it is at a line start, or the start of the line after it.
DocPos SelectedLinesEnd(const State &s, const Cursor &c) {
    const auto end = c.SelectionEnd();
    return end.Index == 0 ? end : NextLine(s, end);
}

void DeleteTextFromAllCursors(State &s, Transaction &tx) {
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].HasSelection()) continue;
        const auto start = items[i].SelectionStart();
        DeleteForCursor(s, tx, i, start, items[i].SelectionEnd());
        Update(items[i], start, false);
    }
}

void InsertTextIntoAllCursors(State &s, Transaction &tx, std::string_view text) {
    DeleteTextFromAllCursors(s, tx);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto end = InsertForCursor(s, tx, i, items[i].SelectionStart(), text);
        Update(items[i], end, false);
    }
}

void AutoIndentAllCursors(State &s, Transaction &tx) {
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart();
        if (items[i].HasSelection()) DeleteForCursor(s, tx, i, start, items[i].SelectionEnd());
        auto index = start.Index;
        const auto lineStart = LineStart(s.Doc, start.Line), lineSize = LineLength(s.Doc, start.Line);
        const auto at = [&](uint64_t k) { return s.Doc[lineStart + k]; };
        const uint64_t previousChar = index > 0 ? at(index - 1) : 0;
        const uint64_t nextChar = index < lineSize ? at(index) : 0;
        if (IsWhiteSpace(nextChar)) {
            while (index < lineSize && IsWhiteSpace(at(index))) ++index;
            DeleteForCursor(s, tx, i, start, {start.Line, index});
        }
        std::string whitespace;
        for (uint64_t k = 0; k < LineLength(s.Doc, start.Line) && IsWhiteSpace(at(k)); ++k) whitespace += Utf8(at(k));
        std::string insert = "\n" + whitespace;
        auto newCursorIndex = whitespace.size();
        if (previousChar == '{' || previousChar == '[') {
            insert += '\t';
            ++newCursorIndex;
            if (nextChar == ToPairCloser(previousChar)) insert += "\n" + whitespace;
        }
        InsertForCursor(s, tx, i, start, insert);
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
    const auto skipCloser = s.CompletePairCloser && s.CompletePairCloser == character && s.CompletePairLocation == CurrentCursor(s.Cursors).SelectionEnd();
    s.CompletePairCloser = 0;
    if (skipCloser) {
        MoveHorizontally(s, false, false, false);
        return;
    }

    auto tx = StartTransaction(s, false);
    const auto opener = character;
    const auto isPaired = !s.Config.Overwrite && s.Config.CompletePairedGlyphs && IsPairOpener(opener);
    const auto closer = ToPairCloser(opener);
    auto &items = s.Cursors.Items;
    if (AnyHasSelection(s.Cursors) && isPaired) {
        for (size_t i = 0; i < items.size(); ++i) {
            if (!items[i].HasSelection()) continue;
            const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
            const auto end1 = InsertText(s, tx, end, Utf8(closer));
            // Upstream passes the selection start as the insert start. See UpstreamBugs.md.
            AdjustForInsert(s.Cursors, i, start, end1);
            InsertForCursor(s, tx, i, start, Utf8(opener));
            Update(items[i], Right(s, start), Right(s, end));
        }
    } else if (isPaired) {
        const auto pair = Utf8(opener) + Utf8(closer);
        for (size_t i = 0; i < items.size(); ++i) {
            const auto start = items[i].SelectionStart();
            InsertForCursor(s, tx, i, start, pair);
            Update(items[i], Right(s, start), false);
        }
        s.CompletePairCloser = uint32_t(closer);
        s.CompletePairLocation = CurrentCursor(s.Cursors).SelectionEnd();
    } else if (!s.Config.Overwrite && s.Config.AutoIndent && character == '\n') {
        AutoIndentAllCursors(s, tx);
    } else {
        if (s.Config.Overwrite) {
            for (size_t i = 0; i < items.size(); ++i) {
                const auto start = items[i].SelectionStart();
                if (!items[i].HasSelection() && start != EndOfLine(s.Doc, start)) DeleteForCursor(s, tx, i, start, Right(s, start));
            }
        }
        InsertTextIntoAllCursors(s, tx, Utf8(character));
    }
    EndTransaction(s, tx);
    if (IsWord(character)) StartAutoComplete(s, false);
}

void HandleDelete(State &s, bool wordMode, bool backward) {
    auto tx = StartTransaction(s, false);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
        if (!items[i].HasSelection()) {
            if (backward) start = Left(s, start, wordMode);
            else end = Right(s, end, wordMode);
        }
        DeleteForCursor(s, tx, i, start, end);
        Update(items[i], start, false);
    }
    EndTransaction(s, tx);
}

void RemoveSelectedLines(State &s) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = StartOfLine(items[i].SelectionStart());
        DeleteForCursor(s, tx, i, start, SelectedLinesEnd(s, items[i]));
        Update(items[i], start, false);
    }
    EndTransaction(s, tx);
}

void InsertLine(State &s, bool above) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = above ? StartOfLine(items[i].SelectionStart()) : SelectedLinesEnd(s, items[i]);
        InsertForCursor(s, tx, i, start, "\n");
        Update(items[i], start, false);
    }
    EndTransaction(s, tx);
}

void IndentLines(State &s, bool deindent) {
    auto tx = StartTransaction(s);
    auto &items = s.Cursors.Items;
    for (size_t i = 0; i < items.size(); ++i) {
        const auto cursorStart = items[i].SelectionStart(), cursorEnd = items[i].SelectionEnd();
        for (auto line = cursorStart.Line; line <= cursorEnd.Line; ++line) {
            const auto size = LineLength(s.Doc, line);
            if (!size || (items[i].HasSelection() && DocPos{line, 0} == cursorEnd)) continue;
            if (deindent) {
                uint64_t column = 0, index = 0;
                const auto lineStart = LineStart(s.Doc, line);
                while (column < s.Config.TabSize && index < size && (s.Doc[lineStart + index] == ' ' || s.Doc[lineStart + index] == '\t')) {
                    column += Columns(s.Config.TabSize, s.Doc[lineStart + index], column);
                    ++index;
                }
                if (index) DeleteForCursor(s, tx, i, {line, 0}, {line, index}, true);
            } else {
                InsertForCursor(s, tx, i, {line, 0}, "\t", true);
            }
        }
    }
    EndTransaction(s, tx);
}

void MoveLines(State &s, bool up) {
    auto &items = s.Cursors.Items;
    if (up ? items.front().SelectionStart().Line == 0 : IsLastLine(s.Doc, items.back().SelectionStart().Line)) return;
    auto tx = StartTransaction(s);
    const auto shift = [up](DocPos pos) { return DocPos{up ? pos.Line - 1 : pos.Line + 1, pos.Index}; };
    for (size_t i = 0; i < items.size(); ++i) {
        const auto start = items[i].SelectionStart(), end = items[i].SelectionEnd();
        const auto deleteStart = StartOfLine(start), deleteEnd = SelectedLinesEnd(s, items[i]);
        const auto text = SectionText(s.Doc, deleteStart, deleteEnd);
        DeleteForCursor(s, tx, i, deleteStart, deleteEnd);
        InsertForCursor(s, tx, i, up ? shift(deleteStart) : NextLine(s, deleteStart), text);
        Update(items[i], shift(start), shift(end));
    }
    EndTransaction(s, tx);
}

void Copy(State &s) {
    std::string text;
    if (AnyHasSelection(s.Cursors)) {
        for (const auto &c : s.Cursors.Items) {
            if (!text.empty()) text += '\n';
            if (c.HasSelection()) text += SelectedText(s.Doc, c);
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

// Replays the edits of `t` on the folds, markers and squiggles, then restores the text and cursors from before or after it.
void Restore(State &s, const Transaction &t, bool undo) {
    const auto &source = undo ? t.Before : t.After;
    auto doc = s.Doc;
    const auto replay = [&](const Edit &e) {
        if (e.Insert == undo) {
            ShiftForDelete(s, e.Start, e.End, doc);
            doc = DeleteText(doc, e.Start, e.End);
        } else {
            const auto from = Offset(source, e.Start);
            doc = vec::Insert(doc, Offset(doc, e.Start), vec::Take(vec::Drop(source, from), Offset(source, e.End) - from));
            ShiftForInsert(s, e.Start, e.End);
        }
    };
    if (undo) std::ranges::for_each(t.Edits | std::views::reverse, replay);
    else std::ranges::for_each(t.Edits, replay);
    s.Doc = source;
    s.Cursors = undo ? t.BeforeCursors : t.AfterCursors;
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

// Selects the first match of the find text at or after `from`, or clears the additional cursors.
void SelectNextOccurrence(State &s, DocPos from) {
    if (const auto match = FindText(s.Doc, from, s.FindText, s.CaseSensitiveFind, s.WholeWordFind)) {
        SetCursor(s.Cursors, match->Start, match->End);
        MakeCursorVisible(s);
    } else {
        ClearAdditional(s.Cursors, true);
    }
}

void SelectAllOccurrencesOf(State &s, std::string_view text, bool caseSensitive, bool wholeWord) {
    const auto first = FindText(s.Doc, {}, text, caseSensitive, wholeWord);
    if (!first) {
        ClearAdditional(s.Cursors, true);
        return;
    }
    SetCursor(s.Cursors, first->Start, first->End);
    while (true) {
        const auto next = FindText(s.Doc, CurrentCursor(s.Cursors).SelectionEnd(), text, caseSensitive, wholeWord);
        if (next->Start == first->Start && next->End == first->End) break;
        AddCursor(s.Cursors, next->Start, next->End);
    }
}

void AddNextOccurrence(State &s, bool wholeWord) {
    const auto &cursor = CurrentCursor(s.Cursors);
    if (const auto match = FindText(s.Doc, cursor.SelectionEnd(), SelectedText(s.Doc, cursor), true, wholeWord)) AddCursor(s.Cursors, match->Start, match->End);
}

void ReplaceTextInCurrentCursor(State &s, std::string_view text) {
    auto tx = StartTransaction(s);
    const auto i = s.Cursors.Current;
    const auto start = s.Cursors.Items[i].SelectionStart();
    DeleteForCursor(s, tx, i, start, s.Cursors.Items[i].SelectionEnd());
    const auto end = InsertForCursor(s, tx, i, start, text);
    Update(s.Cursors.Items[i], end, false);
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
    const auto &cursor = MainCursor(s.Cursors);
    if (cursor.HasSelection()) {
        if (cursor.SelectionStart().Line == cursor.SelectionEnd().Line) s.FindText = SelectedText(s.Doc, cursor);
    } else {
        const auto word = WholeWord(s.Doc, cursor.SelectionStart(), true);
        if (word.Start != word.End) s.FindText = SectionText(s.Doc, word.Start, word.End);
    }
    s.FindReplaceVisible = true;
    s.FocusOnFind = true;
}

void FocusEditor(State &s) {
    s.FocusOnEditor = true;
    s.FocusOnFind = false;
}

void CloseFindReplace(State &s) {
    if (!s.FindReplaceVisible) return;
    s.FindReplaceVisible = false;
    FocusEditor(s);
}

void Find(State &s) {
    if (s.FindText.empty()) return;
    SelectNextOccurrence(s, CurrentCursor(s.Cursors).SelectionEnd());
    FocusEditor(s);
}

void FindAll(State &s) {
    if (s.FindText.empty()) return;
    SelectAllOccurrencesOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    FocusEditor(s);
}

void Replace(State &s) {
    if (s.FindText.empty()) return;
    if (!AnyHasSelection(s.Cursors)) SelectNextOccurrence(s, CurrentCursor(s.Cursors).SelectionEnd());
    ReplaceTextInCurrentCursor(s, s.ReplaceText);
    SelectNextOccurrence(s, CurrentCursor(s.Cursors).SelectionEnd());
    FocusEditor(s);
}

void ReplaceAll(State &s) {
    if (s.FindText.empty()) return;
    SelectAllOccurrencesOf(s, s.FindText, s.CaseSensitiveFind, s.WholeWordFind);
    ReplaceTextInAllCursors(s, s.ReplaceText);
    FocusEditor(s);
}

// Sets the squiggle of the glyphs from `start` to `end`.
void SetSquiggle(State &s, DocPos start, DocPos end, uint64_t index) {
    auto t = s.GlyphSquiggles.transient();
    for (auto i = Offset(s.Doc, start), last = Offset(s.Doc, end); i < last; ++i) t.set(i, index);
    s.GlyphSquiggles = std::move(t).persistent();
    s.DeletesHappened = true; // Compress unused squiggles next frame.
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
        const auto color = left.Index < LineLength(s.Doc, left.Line) ? ColorAt(s, Offset(s.Doc, left)) : Color::Text;
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
    if (start >= end) return;
    if (s.GlyphSquiggles.Size != s.Doc.Size) s.GlyphSquiggles = Zeros(s.Doc.Size);
    s.Squiggles.push_back({type, color, std::string(tooltip), style});
    SetSquiggle(s, start, end, s.Squiggles.size());
}

void ClearSquiggles(State &s, DocPos start, DocPos end) {
    if (s.GlyphSquiggles.Size == s.Doc.Size) SetSquiggle(s, Normalize(s.Doc, start), Normalize(s.Doc, end), 0);
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
        case Op::Paste: ReplaceTextInAllCursors(s, a.Text); break;
        case Op::Undo:
            if (s.UndoIndex > 0) Restore(s, s.History[--s.UndoIndex], true);
            break;
        case Op::Redo:
            if (s.UndoIndex < s.History.size()) Restore(s, s.History[s.UndoIndex++], false);
            break;
        case Op::Delete: HandleDelete(s, a.Word, false); break;
        case Op::Backspace: HandleDelete(s, a.Word, true); break;
        case Op::RemoveSelectedLines: RemoveSelectedLines(s); break;
        case Op::DeindentLines: IndentLines(s, true); break;
        case Op::IndentLines: IndentLines(s, false); break;
        case Op::MoveUpLines: MoveLines(s, true); break;
        case Op::MoveDownLines: MoveLines(s, false); break;
        case Op::ToggleOverwrite: s.Config.Overwrite = !s.Config.Overwrite; break;
        case Op::Character: HandleCharacter(s, a.Codepoint); break;
        case Op::InsertLineBelow: InsertLine(s, false); break;
        case Op::InsertLineAbove: InsertLine(s, true); break;
        case Op::Tab:
            if (AnyHasSelection(s.Cursors)) IndentLines(s, false);
            else HandleCharacter(s, '\t');
            break;
        case Op::ClearAdditionalCursors: ClearAdditional(s.Cursors); break;
        case Op::AddNextOccurrence: AddNextOccurrence(s, a.Word); break;
        case Op::SelectAllOccurrences: SelectAllOccurrencesOf(s, SelectedText(s.Doc, CurrentCursor(s.Cursors)), true, a.Word); break;
        case Op::OpenFindReplace: OpenFindReplace(s); break;
        case Op::CloseFindReplace: CloseFindReplace(s); break;
        case Op::Find: Find(s); break;
        case Op::FindAll: FindAll(s); break;
        case Op::Replace: Replace(s); break;
        case Op::ReplaceAll: ReplaceAll(s); break;
        case Op::SelectFirstOccurrence:
            s.FindText = a.Text;
            if (!s.FindText.empty()) SelectNextOccurrence(s, {});
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
        case Op::PanBy: s.ScrollTarget = s.Scroll - a.Vec; break;
        case Op::StopScrollMode: s.Scrolling = false; break;
        case Op::DragLineSelect: {
            auto &c = CurrentCursor(s.Cursors);
            Update(c, c.End < c.Start ? StartOfLine(a.Pos) : Normalize(s.Doc, {a.Pos.Line + 1, 0}));
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
        case Op::TripleClick: Update(CurrentCursor(s.Cursors), StartOfLine(a.Pos), Normalize(s.Doc, {a.Pos.Line + 1, 0})); break;
        case Op::DoubleClick: {
            const auto cp = CodepointAt(s.Doc, a.Pos);
            const Bracket *b = nullptr;
            if (IsBracketOpener(cp)) b = EnclosingBrackets(s.Derived, Right(s, a.Pos));
            else if (IsBracketCloser(cp)) b = EnclosingBrackets(s.Derived, a.Pos);
            if (b && IsBracketOpener(cp) && !a.Select) {
                SetCursor(s.Cursors, Right(s, b->Start), b->End);
            } else if (b) {
                SetCursor(s.Cursors, b->Start, Right(s, b->End));
            } else if (a.Pos.Index != LineLength(s.Doc, a.Pos.Line)) {
                const auto word = WholeWord(s.Doc, a.Pos);
                if (word.Start != word.End) Update(CurrentCursor(s.Cursors), word.Start, word.End);
            }
            break;
        }
        case Op::ClickLineNumbers: {
            const auto start = StartOfLine(a.Pos), end = Normalize(s.Doc, {a.Pos.Line + 1, 0});
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
                AddCursor(s.Cursors, a.Pos, a.Pos);
                CancelAutoComplete(s);
            } else {
                SetCursor(s.Cursors, a.Pos, a.Pos);
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
            s.ScrollTarget = {FLT_MAX, std::clamp(s.MiniMapScrollY + documentOffset, 0.0f, s.TotalSize.y - a.Vec.x)};
            break;
        }
        case Op::ClickMiniMap:
            if (a.Select) {
                s.MiniMapIsScrollbar = true;
                s.MiniMapScrollStart = a.Vec.y;
                s.MiniMapScrollY = s.Scroll.y;
            } else {
                ScrollToLine(s, a.Count, State::Align::Middle);
            }
            break;
        case Op::AutoCompleteShortcut: StartAutoComplete(s, true); break;
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
