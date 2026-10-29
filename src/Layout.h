#pragma once

#include "TextEdit.h"

namespace ipte {
struct VisPos {
    uint64_t Row{0}, Column{0};
};

uint32_t Columns(const Settings &config, uint64_t cp, uint64_t column);
// Updates `s.Folds` and `s.LineFolds`. Returns whether folding changed.
bool RefreshFolds(State &s, bool bracketsChanged);
// Updates `s.Layout` if its inputs changed.
void RefreshLayout(State &s);
// Updates `s.MiniRows` if its inputs changed.
void RefreshMiniMap(State &s);
inline uint64_t RowCount(const State &s) { return s.Layout.TotalRows; }
inline const LineLayout &LineAt(const State &s, uint64_t line) { return s.Layout.Lines[line]; }
inline bool IsHidden(const State &s, uint64_t line) { return line < s.LineFolds.size() && s.LineFolds[line] == FoldingState::Hidden; }
inline FoldingState FoldAt(const State &s, uint64_t line) { return line < s.LineFolds.size() ? s.LineFolds[line] : FoldingState::Visible; }
VisPos DocToVis(const State &s, DocPos pos);
DocPos VisToDoc(const State &s, VisPos pos);
VisPos NormalizeVis(const State &s, VisPos pos);
bool IsVisPosOverGlyph(const State &s, VisPos pos);
// Converts a point in glyph units to the glyph under it and the nearest caret position.
void ScreenToDoc(const State &s, ImVec2 screenPos, DocPos &glyphPos, DocPos &cursorPos);
// Steps to the previous or next line, skipping hidden lines.
DocPos LeftBeforeHiddenLines(const State &s, DocPos pos);
DocPos RightAfterHiddenLines(const State &s, DocPos pos);
} // namespace ipte
