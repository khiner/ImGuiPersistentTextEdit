#pragma once

#include "TextEdit.h"

#include <utility>

namespace ipte {
struct VisPos {
    uint64_t Row{0}, Column{0};
};

uint32_t Columns(uint32_t tabSize, uint64_t cp, uint64_t column);
// Updates `s.Folds` and `s.LineFolds`. Returns whether folding changed.
bool RefreshFolds(State &s, bool bracketsChanged);
// Updates `s.Layout` if its inputs changed.
void RefreshLayout(State &s);
// Updates `s.MiniRows` if its inputs changed.
void RefreshMiniMap(State &s);
inline uint64_t RowCount(const State &s) { return s.Layout.Rows.size(); }
inline const LineLayout &LineAt(const State &s, uint64_t line) { return s.Layout.Lines[line]; }
inline bool IsHidden(const State &s, uint64_t line) { return line < s.LineFolds.size() && s.LineFolds[line] == FoldingState::Hidden; }
inline FoldingState FoldAt(const State &s, uint64_t line) { return line < s.LineFolds.size() ? s.LineFolds[line] : FoldingState::Visible; }
VisPos DocToVis(const State &s, DocPos pos);
DocPos VisToDoc(const State &s, VisPos pos);
VisPos NormalizeVis(const State &s, VisPos pos);
// Returns the glyph under a point in glyph units and the nearest caret position.
std::pair<DocPos, DocPos> ScreenToDoc(const State &s, ImVec2 screenPos);
// Steps to the previous or next line, skipping hidden lines.
DocPos LeftBeforeHiddenLines(const State &s, DocPos pos);
DocPos RightAfterHiddenLines(const State &s, DocPos pos);

// Steps over the glyphs of a layout row. `Column` is the row column of glyph `Index`, and `LineColumn` its column from the line start, for tabs.
struct RowWalk {
    const State &S;
    uint64_t Start, Index{0}, End{0}, Column{0}, LineColumn{0};

    RowWalk(const State &s, uint64_t line, uint64_t section);

    void Step() {
        const auto w = Columns(S.Config.TabSize, S.Doc[Start + Index++], LineColumn);
        LineColumn += w;
        Column += w;
    }

    // Steps past the glyph whose right edge reaches `x`. Returns its left column.
    uint64_t StepTo(auto x) {
        auto left = Column;
        while (Column < x && Index < End) {
            left = Column;
            Step();
        }
        return left;
    }
};
} // namespace ipte
