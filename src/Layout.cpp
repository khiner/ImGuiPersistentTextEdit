#include "Layout.h"

#include "Document.h"
#include "LineBreak.h"

#include <algorithm>
#include <unordered_set>

namespace ipte {
uint32_t Columns(const Settings &config, uint64_t cp, uint64_t column) {
    if (cp == '\t') return uint32_t(config.TabSize - (column % config.TabSize));
    return uint32_t(GlyphWidth(cp));
}

bool RefreshFolds(State &s, bool bracketsChanged) {
    bool updated = s.FoldsDirty;
    s.FoldsDirty = false;
    if (s.LineFoldsFor != s.Config.LineFolding) {
        s.LineFoldsFor = s.Config.LineFolding;
        if (!s.Config.LineFolding) {
            s.Folds.clear();
            s.Folded.clear();
        }
        updated = true;
    }
    if (bracketsChanged) updated = true;

    const auto lines = LineCount(s.Doc);
    if (s.Config.LineFolding && updated) {
        s.Folds.clear();
        std::unordered_set<uint64_t> previouslyFolded;
        for (const auto &b : s.Derived.Brackets) {
            if (b.End.Line - b.Start.Line >= 1) {
                if (std::ranges::find(s.Folded, b.Start.Line) != s.Folded.end()) previouslyFolded.insert(b.Start.Line);
                s.Folds.push_back({b.Start.Line, b.End.Line});
            }
        }
        s.Folded.assign(previouslyFolded.begin(), previouslyFolded.end());
        std::ranges::sort(s.Folded);
        ++s.FoldsVersion;
    }
    if (s.LineFolds.size() != lines || updated) {
        s.LineFolds.assign(lines, FoldingState::Visible);
        for (const auto &fold : s.Folds) {
            if (std::ranges::find(s.Folded, fold.Start) != s.Folded.end()) {
                s.LineFolds[fold.Start] = FoldingState::Folded;
                for (auto i = fold.Start + 1; i <= fold.End; ++i) s.LineFolds[i] = FoldingState::Hidden;
            } else if (s.LineFolds[fold.Start] != FoldingState::Hidden) {
                s.LineFolds[fold.Start] = FoldingState::Foldable;
            }
        }
    }
    return updated;
}

namespace {
void Classify(const State &s, const std::vector<uint64_t> &line, std::vector<BreakOption> &breaks) {
    const auto size = line.size();
    breaks.assign(size, BreakOption::Undefined);
    if (size == 0) return;
    if (size == 1) {
        breaks[0] = BreakOption::NoBreak;
        return;
    }
    const auto &config = s.Config.LineBreak;
    if (config.UseUnicodeAnnex14) {
        ClassifyAnnex14(config, line.data(), size, breaks.data());
    } else {
        const auto after = Codepoints(config.BreakAfter), before = Codepoints(config.BreakBefore);
        const auto in = [](const std::vector<uint64_t> &set, uint64_t cp) { return std::ranges::find(set, cp) != set.end(); };
        for (size_t i = 0; i < size; ++i) {
            if (in(after, line[i])) {
                breaks[i] = BreakOption::AllowBreak;
            } else if (i && in(before, line[i])) {
                breaks[i - 1] = BreakOption::AllowBreak;
                breaks[i] = BreakOption::NoBreak;
            } else {
                breaks[i] = BreakOption::NoBreak;
            }
        }
    }
    breaks[size - 1] = BreakOption::NoBreak;
}

void TypesetLine(State &s, uint64_t index, std::vector<uint64_t> &line, std::vector<uint32_t> &widths, std::vector<BreakOption> &breaks) {
    auto &layout = s.Layout.Lines[index];
    auto &sections = s.Layout.Sections;
    const auto start = LineStart(s.Doc, index), end = LineEnd(s.Doc, index);
    line.assign(end - start, 0);
    widths.assign(end - start, 0);
    uint64_t columns = 0;
    for (auto k = start; k < end; ++k) {
        line[k - start] = s.Doc[k];
        widths[k - start] = Columns(s.Config, s.Doc[k], columns);
        columns += widths[k - start];
    }
    layout.Rows = 1;
    layout.FirstSection = InvalidLine;
    if (!s.Config.WordWrap) {
        layout.Columns = columns;
        return;
    }

    Classify(s, line, breaks);
    const auto wordWrapColumns = s.WordWrapColumns;
    std::vector<Section> found;
    uint64_t lastBreakIndex = 0, lastBreakableIndex = 0, lastBreakableColumn = 0, indent = 0;
    columns = 0;
    bool isAtBeginning = true, isFirstRow = true;
    const auto size = line.size();
    size_t i = 0;
    while (i < size) {
        if (breaks[i] == BreakOption::MustBreak) {
            ++layout.Rows;
            found.push_back({lastBreakIndex, i, columns, indent});
            columns = 0;
            indent = 0;
            isAtBeginning = true;
            isFirstRow = true;
            lastBreakIndex = i + 1;
            lastBreakableIndex = i + 1;
        } else {
            if (isAtBeginning) {
                if (IsWhiteSpace(line[i])) indent += widths[i];
                else isAtBeginning = false;
            }
            columns += widths[i];
            if (columns < wordWrapColumns) {
                if (breaks[i] == BreakOption::AllowBreak) {
                    lastBreakableIndex = i + 1;
                    lastBreakableColumn = columns;
                }
                ++i;
            } else {
                if (lastBreakableIndex == lastBreakIndex) {
                    lastBreakableIndex = i + 1;
                    found.push_back({lastBreakIndex, lastBreakableIndex, columns, isFirstRow ? 0 : indent});
                } else {
                    found.push_back({lastBreakIndex, lastBreakableIndex, lastBreakableColumn, isFirstRow ? 0 : indent});
                }
                i = lastBreakableIndex;
                ++layout.Rows;
                columns = indent;
                lastBreakIndex = lastBreakableIndex;
                lastBreakableColumn = indent;
                isFirstRow = false;
            }
        }
    }
    if (layout.Rows > 1) {
        found.push_back({lastBreakIndex, size, columns, indent});
        layout.FirstSection = sections.size();
        layout.Columns = 0;
        for (const auto &section : found) {
            sections.push_back(section);
            layout.Columns = std::max(layout.Columns, section.Columns);
        }
    } else {
        layout.Columns = columns;
    }
}

// Sums glyph widths in [from, to). `lineColumn` is the running column from the line start, for tabs.
uint64_t ColumnsBefore(const State &s, uint64_t lineStart, uint64_t from, uint64_t to, uint64_t &lineColumn) {
    uint64_t columns = 0;
    for (auto i = from; i < to; ++i) {
        const auto w = Columns(s.Config, s.Doc[lineStart + i], lineColumn);
        lineColumn += w;
        columns += w;
    }
    return columns;
}
} // namespace

void RefreshLayout(State &s) {
    auto &t = s.Layout;
    if (t.Root == s.Doc.Root && t.Tail == s.Doc.Tail && t.Size == s.Doc.Size && t.TabSize == s.Config.TabSize && t.WordWrap == s.Config.WordWrap && t.WordWrapColumns == s.WordWrapColumns && t.LineBreak == s.Config.LineBreak && t.FoldsVersion == s.FoldsVersion) return;
    t.Root = s.Doc.Root;
    t.Tail = s.Doc.Tail;
    t.Size = s.Doc.Size;
    t.TabSize = s.Config.TabSize;
    t.WordWrap = s.Config.WordWrap;
    t.WordWrapColumns = s.WordWrapColumns;
    t.LineBreak = s.Config.LineBreak;
    t.FoldsVersion = s.FoldsVersion;
    ++t.Version;

    const auto lines = LineCount(s.Doc);
    t.Lines.assign(lines, {});
    t.Sections.clear();
    t.Rows.clear();
    t.TotalRows = 0;
    t.TotalColumns = 0;
    std::vector<uint64_t> line;
    std::vector<uint32_t> widths;
    std::vector<BreakOption> breaks;
    for (uint64_t i = 0; i < lines; ++i) {
        TypesetLine(s, i, line, widths, breaks);
        auto &layout = t.Lines[i];
        layout.Row = t.TotalRows;
        if (IsHidden(s, i)) continue;
        t.TotalRows += layout.Rows;
        t.TotalColumns = std::max(t.TotalColumns, layout.Columns);
        if (layout.FirstSection != InvalidLine) {
            for (uint64_t k = 0; k < layout.Rows; ++k) t.Rows.push_back({i, k, t.Sections[layout.FirstSection + k].Columns});
        } else {
            t.Rows.push_back({i, 0, layout.Columns});
        }
    }
}

VisPos DocToVis(const State &s, DocPos pos) {
    const auto &t = s.Layout;
    if (t.Rows.empty() || pos.Line >= t.Lines.size()) return {};
    const auto &line = t.Lines[pos.Line];
    VisPos vis{line.Row, 0};
    const auto lineStart = LineStart(s.Doc, pos.Line);
    if (IsHidden(s, pos.Line)) {
        vis.Column = line.Row < t.Rows.size() ? t.Rows[line.Row].Columns : 0;
    } else if (line.FirstSection != InvalidLine) {
        for (uint64_t k = 0; k < line.Rows; ++k) {
            const auto &section = t.Sections[line.FirstSection + k];
            if (pos.Index >= section.StartIndex && pos.Index <= section.EndIndex) {
                uint64_t lineColumn = 0;
                ColumnsBefore(s, lineStart, 0, section.StartIndex, lineColumn);
                vis.Column = section.Indent + ColumnsBefore(s, lineStart, section.StartIndex, pos.Index, lineColumn);
                break;
            }
            ++vis.Row;
        }
    } else {
        uint64_t lineColumn = 0;
        vis.Column = ColumnsBefore(s, lineStart, 0, pos.Index, lineColumn);
    }
    return vis;
}

DocPos VisToDoc(const State &s, VisPos pos) {
    const auto &t = s.Layout;
    if (pos.Row >= t.Rows.size()) return {};
    const auto &row = t.Rows[pos.Row];
    if (row.Line >= LineCount(s.Doc)) return {};
    const auto &line = t.Lines[row.Line];
    const auto lineStart = LineStart(s.Doc, row.Line);
    uint64_t index, left, right, end, lineColumn = 0;
    if (line.FirstSection != InvalidLine) {
        const auto &section = t.Sections[line.FirstSection + row.Section];
        index = section.StartIndex;
        left = right = section.Indent;
        end = section.EndIndex;
        ColumnsBefore(s, lineStart, 0, section.StartIndex, lineColumn);
    } else {
        index = left = right = 0;
        end = LineLength(s.Doc, row.Line);
    }
    while (right < pos.Column && index < end) {
        left = right;
        const auto w = Columns(s.Config, s.Doc[lineStart + index], lineColumn);
        lineColumn += w;
        right += w;
        ++index;
    }
    if (right - left <= 1) return {row.Line, index};
    return {row.Line, pos.Column - left <= right - pos.Column ? index - 1 : index};
}

VisPos NormalizeVis(const State &s, VisPos pos) {
    const auto &t = s.Layout;
    if (t.TotalRows == 0) return {};
    if (pos.Row >= t.TotalRows) return {t.TotalRows - 1, t.Rows[t.TotalRows - 1].Columns};
    if (pos.Column > t.Rows[pos.Row].Columns) return {pos.Row, t.Rows[pos.Row].Columns};
    return pos;
}

bool IsVisPosOverGlyph(const State &s, VisPos pos) { return pos.Row < s.Layout.TotalRows && pos.Column <= s.Layout.Rows[pos.Row].Columns; }

void ScreenToDoc(const State &s, ImVec2 screenPos, DocPos &glyphPos, DocPos &cursorPos) {
    const auto &t = s.Layout;
    const auto colNo = uint64_t(screenPos.x), rowNo = uint64_t(screenPos.y);
    if (screenPos.y <= 0.0f) {
        glyphPos = cursorPos = {};
    } else if (rowNo >= t.TotalRows) {
        glyphPos = cursorPos = Bottom(s.Doc);
    } else {
        const auto &row = t.Rows[rowNo];
        const auto &line = t.Lines[row.Line];
        const auto lineStart = LineStart(s.Doc, row.Line);
        if (screenPos.x <= 0.0f) {
            glyphPos = cursorPos = {row.Line, 0};
        } else if (colNo > row.Columns) {
            if (line.FirstSection != InvalidLine) glyphPos = {row.Line, t.Sections[line.FirstSection + row.Section].EndIndex};
            else glyphPos = EndOfLine(s.Doc, {row.Line, 0});
            cursorPos = glyphPos;
        } else {
            uint64_t index, left, right, end, lineColumn = 0;
            if (line.FirstSection != InvalidLine) {
                const auto &section = t.Sections[line.FirstSection + row.Section];
                index = section.StartIndex;
                left = right = section.Indent;
                end = section.EndIndex;
                ColumnsBefore(s, lineStart, 0, section.StartIndex, lineColumn);
            } else {
                index = left = right = 0;
                end = LineLength(s.Doc, row.Line);
            }
            while (float(right) < screenPos.x && index < end) {
                left = right;
                const auto w = Columns(s.Config, s.Doc[lineStart + index], lineColumn);
                lineColumn += w;
                right += w;
                ++index;
            }
            const auto leftDiff = screenPos.x - float(left), rightDiff = float(right) - screenPos.x;
            glyphPos = {row.Line, left == right ? index : index - 1};
            cursorPos = {row.Line, leftDiff <= rightDiff ? index - 1 : index};
        }
    }
}

void RefreshMiniMap(State &s) {
    const bool configChanged = s.MiniRowsFor != s.Config.ShowMiniMap;
    s.MiniRowsFor = s.Config.ShowMiniMap;
    if (!configChanged && s.MiniRowsLayout == s.Layout.Version && s.MiniRowsColors == s.Derived.Version) return;
    s.MiniRowsLayout = s.Layout.Version;
    s.MiniRowsColors = s.Derived.Version;
    s.MiniRows.clear();
    const auto &t = s.Layout;
    for (uint64_t i = 0; i < t.TotalRows; ++i) {
        const auto &row = t.Rows[i];
        const auto &line = t.Lines[row.Line];
        const auto lineStart = LineStart(s.Doc, row.Line);
        uint64_t index = 0, column = 0, endColumn = line.Columns, lineColumn = 0;
        if (s.Config.WordWrap && line.FirstSection != InvalidLine) {
            const auto &section = t.Sections[line.FirstSection + row.Section];
            index = section.StartIndex;
            column = section.Indent;
            endColumn = section.Columns;
            for (uint64_t k = 0; k < index; ++k) lineColumn += Columns(s.Config, s.Doc[lineStart + k], lineColumn);
        }
        auto &mini = s.MiniRows.emplace_back();
        auto start = column;
        auto color = Color::Background;
        while (column < endColumn) {
            const auto offset = lineStart + index++;
            const auto glyphColor = Color(s.Derived.Colors[offset]);
            if (glyphColor != color) {
                if (column != start) {
                    if (color != Color::Whitespace) mini.Sections.push_back({start, column, color});
                    start = column;
                }
                color = glyphColor;
            }
            const auto w = Columns(s.Config, s.Doc[offset], lineColumn);
            lineColumn += w;
            column += w;
        }
        if (column != start && color != Color::Whitespace) mini.Sections.push_back({start, column, color});
    }
}

DocPos LeftBeforeHiddenLines(const State &s, DocPos pos) {
    if (pos.Line == 0) return pos;
    auto line = pos.Line - 1;
    while (line > 0 && IsHidden(s, line)) --line;
    return {line, LineLength(s.Doc, line)};
}

DocPos RightAfterHiddenLines(const State &s, DocPos pos) {
    const auto last = LineCount(s.Doc) - 1;
    if (pos.Line == last) return pos;
    auto line = pos.Line + 1;
    while (line < last && IsHidden(s, line)) ++line;
    return {line, 0};
}
} // namespace ipte
