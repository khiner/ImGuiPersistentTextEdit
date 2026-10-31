#include "Layout.h"

#include "Colorize.h"
#include "Document.h"
#include "LineBreak.h"

#include <algorithm>
#include <span>
#include <tuple>

namespace ipte {
uint32_t Columns(uint32_t tabSize, uint64_t cp, uint64_t column) {
    if (cp == '\t') return uint32_t(tabSize - (column % tabSize));
    return uint32_t(GlyphWidth(cp));
}

bool RefreshFolds(State &s, bool bracketsChanged) {
    bool updated = s.FoldsDirty || bracketsChanged;
    s.FoldsDirty = false;
    if (s.LineFoldsFor != s.Config.LineFolding) {
        s.LineFoldsFor = s.Config.LineFolding;
        if (!s.Config.LineFolding) {
            s.Folds.clear();
            s.Folded.clear();
        }
        updated = true;
    }

    if (s.Config.LineFolding && updated) {
        s.Folds.clear();
        for (const auto &b : s.Derived.Brackets)
            if (b.End.Line != b.Start.Line) s.Folds.push_back({b.Start.Line, b.End.Line});
        std::erase_if(s.Folded, [&](uint64_t line) { return !std::ranges::contains(s.Folds, line, &Fold::Start); });
        std::ranges::sort(s.Folded);
        s.Folded.erase(std::ranges::unique(s.Folded).begin(), s.Folded.end());
        ++s.FoldsVersion;
    }
    const auto lines = LineCount(s.Doc);
    if (s.LineFolds.size() != lines || updated) {
        s.LineFolds.assign(lines, FoldingState::Visible);
        for (const auto &fold : s.Folds) {
            if (std::ranges::contains(s.Folded, fold.Start)) {
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
void Classify(const LineBreakConfig &config, const std::vector<uint64_t> &line, std::vector<BreakOption> &breaks) {
    const auto size = line.size();
    breaks.assign(size, BreakOption::Undefined);
    if (size == 0) return;
    if (size == 1) {
        breaks[0] = BreakOption::NoBreak;
        return;
    }
    if (config.UseUnicodeAnnex14) {
        ClassifyAnnex14(config, line.data(), size, breaks.data());
    } else {
        const auto after = Codepoints(config.BreakAfter), before = Codepoints(config.BreakBefore);
        for (size_t i = 0; i < size; ++i) {
            if (std::ranges::contains(after, line[i])) {
                breaks[i] = BreakOption::AllowBreak;
            } else if (i && std::ranges::contains(before, line[i])) {
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
    LineCodepoints(s.Doc, index, line);
    widths.clear();
    uint64_t columns = 0;
    for (const auto cp : line) {
        widths.push_back(Columns(s.Config.TabSize, cp, columns));
        columns += widths.back();
    }
    layout.Columns = columns;
    if (!s.Config.WordWrap) return;

    Classify(s.Config.LineBreak, line, breaks);
    const auto first = sections.size();
    uint64_t lastBreakIndex = 0, lastBreakableIndex = 0, lastBreakableColumn = 0, indent = 0;
    columns = 0;
    bool isAtBeginning = true, isFirstRow = true;
    const auto size = line.size();
    size_t i = 0;
    while (i < size) {
        if (breaks[i] == BreakOption::MustBreak) {
            sections.push_back({lastBreakIndex, i, columns, indent});
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
            if (columns < s.WordWrapColumns) {
                if (breaks[i] == BreakOption::AllowBreak) {
                    lastBreakableIndex = i + 1;
                    lastBreakableColumn = columns;
                }
                ++i;
            } else {
                if (lastBreakableIndex == lastBreakIndex) {
                    lastBreakableIndex = i + 1;
                    lastBreakableColumn = columns;
                }
                sections.push_back({lastBreakIndex, lastBreakableIndex, lastBreakableColumn, isFirstRow ? 0 : indent});
                i = lastBreakableIndex;
                columns = indent;
                lastBreakIndex = lastBreakableIndex;
                lastBreakableColumn = indent;
                isFirstRow = false;
            }
        }
    }
    if (sections.size() == first) return;

    sections.push_back({lastBreakIndex, size, columns, indent});
    layout.FirstSection = first;
    layout.Rows = sections.size() - first;
    layout.Columns = std::ranges::max(std::span(sections).subspan(first), {}, &Section::Columns).Columns;
}
} // namespace

RowWalk::RowWalk(const State &s, uint64_t line, uint64_t section) : S(s), Start(LineStart(s.Doc, line)) {
    const auto first = s.Layout.Lines[line].FirstSection;
    if (first == InvalidLine) {
        End = LineLength(s.Doc, line);
        return;
    }
    const auto &sec = s.Layout.Sections[first + section];
    End = sec.EndIndex;
    while (Index < sec.StartIndex) Step();
    Column = sec.Indent;
}

void RefreshLayout(State &s) {
    auto &t = s.Layout;
    auto key = std::tie(t.Root, t.Tail, t.Size, t.TabSize, t.WordWrap, t.WordWrapColumns, t.LineBreak, t.FoldsVersion);
    const auto current = std::tie(s.Doc.Root, s.Doc.Tail, s.Doc.Size, s.Config.TabSize, s.Config.WordWrap, s.WordWrapColumns, s.Config.LineBreak, s.FoldsVersion);
    if (key == current) return;
    key = current;
    ++t.Version;

    const auto lines = LineCount(s.Doc);
    t.Lines.assign(lines, {});
    t.Sections.clear();
    t.Rows.clear();
    t.TotalColumns = 0;
    std::vector<uint64_t> line;
    std::vector<uint32_t> widths;
    std::vector<BreakOption> breaks;
    for (uint64_t i = 0; i < lines; ++i) {
        TypesetLine(s, i, line, widths, breaks);
        auto &layout = t.Lines[i];
        layout.Row = t.Rows.size();
        if (IsHidden(s, i)) continue;
        t.TotalColumns = std::max(t.TotalColumns, layout.Columns);
        for (uint64_t k = 0; k < layout.Rows; ++k) t.Rows.push_back({i, k, layout.FirstSection == InvalidLine ? layout.Columns : t.Sections[layout.FirstSection + k].Columns});
    }
}

VisPos DocToVis(const State &s, DocPos pos) {
    const auto &t = s.Layout;
    if (t.Rows.empty() || pos.Line >= t.Lines.size()) return {};
    const auto &line = t.Lines[pos.Line];
    if (IsHidden(s, pos.Line)) return {line.Row, line.Row < t.Rows.size() ? t.Rows[line.Row].Columns : 0};
    uint64_t k = 0;
    if (line.FirstSection != InvalidLine) {
        for (; k < line.Rows; ++k) {
            const auto &section = t.Sections[line.FirstSection + k];
            if (pos.Index >= section.StartIndex && pos.Index <= section.EndIndex) break;
        }
        if (k == line.Rows) return {line.Row + k, 0};
    }
    RowWalk walk(s, pos.Line, k);
    while (walk.Index < pos.Index) walk.Step();
    return {line.Row + k, walk.Column};
}

DocPos VisToDoc(const State &s, VisPos pos) {
    if (pos.Row >= s.Layout.Rows.size()) return {};
    const auto &row = s.Layout.Rows[pos.Row];
    if (row.Line >= LineCount(s.Doc)) return {};
    RowWalk walk(s, row.Line, row.Section);
    const auto left = walk.StepTo(pos.Column), right = walk.Column;
    if (right - left <= 1) return {row.Line, walk.Index};
    return {row.Line, pos.Column - left <= right - pos.Column ? walk.Index - 1 : walk.Index};
}

VisPos NormalizeVis(const State &s, VisPos pos) {
    const auto &rows = s.Layout.Rows;
    if (rows.empty()) return {};
    if (pos.Row >= rows.size()) return {rows.size() - 1, rows.back().Columns};
    if (pos.Column > rows[pos.Row].Columns) return {pos.Row, rows[pos.Row].Columns};
    return pos;
}

std::pair<DocPos, DocPos> ScreenToDoc(const State &s, ImVec2 screenPos) {
    if (screenPos.y <= 0.0f) return {};
    const auto rowNo = uint64_t(screenPos.y);
    if (rowNo >= s.Layout.Rows.size()) return {Bottom(s.Doc), Bottom(s.Doc)};
    const auto &row = s.Layout.Rows[rowNo];
    if (screenPos.x <= 0.0f) return {{row.Line, 0}, {row.Line, 0}};
    RowWalk walk(s, row.Line, row.Section);
    if (uint64_t(screenPos.x) > row.Columns) return {{row.Line, walk.End}, {row.Line, walk.End}};
    const auto left = walk.StepTo(screenPos.x), right = walk.Column;
    const auto leftDiff = screenPos.x - float(left), rightDiff = float(right) - screenPos.x;
    const DocPos glyphPos{row.Line, left == right ? walk.Index : walk.Index - 1};
    const DocPos cursorPos{row.Line, leftDiff <= rightDiff ? walk.Index - 1 : walk.Index};
    return {glyphPos, cursorPos};
}

void RefreshMiniMap(State &s) {
    if (s.MiniRowsLayout == s.Layout.Version && s.MiniRowsColors == s.Derived.Version) return;
    s.MiniRowsLayout = s.Layout.Version;
    s.MiniRowsColors = s.Derived.Version;
    s.MiniRows.clear();
    for (const auto &row : s.Layout.Rows) {
        RowWalk walk(s, row.Line, row.Section);
        auto &mini = s.MiniRows.emplace_back();
        auto start = walk.Column;
        auto color = Color::Background;
        while (walk.Column < row.Columns) {
            const auto glyphColor = ColorAt(s, walk.Start + walk.Index);
            if (glyphColor != color) {
                if (walk.Column != start) {
                    if (color != Color::Whitespace) mini.Sections.push_back({start, walk.Column, color});
                    start = walk.Column;
                }
                color = glyphColor;
            }
            walk.Step();
        }
        if (walk.Column != start && color != Color::Whitespace) mini.Sections.push_back({start, walk.Column, color});
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
