#include "Document.h"

#include "Layout.h"

#include <algorithm>

namespace ipte {
bool IsPairOpener(uint64_t cp) { return cp == '{' || cp == '[' || cp == '(' || cp == '\'' || cp == '"'; }
bool IsBracketOpener(uint64_t cp) { return cp == '{' || cp == '[' || cp == '('; }
bool IsBracketCloser(uint64_t cp) { return cp == '}' || cp == ']' || cp == ')'; }
uint64_t ToPairCloser(uint64_t cp) {
    switch (cp) {
        case '{': return '}';
        case '[': return ']';
        case '(': return ')';
        default: return cp;
    }
}
uint64_t ToPairOpener(uint64_t cp) {
    switch (cp) {
        case '}': return '{';
        case ']': return '[';
        case ')': return '(';
        default: return cp;
    }
}

std::vector<uint64_t> Codepoints(std::string_view utf8) {
    std::vector<uint64_t> out;
    const auto *i = reinterpret_cast<const unsigned char *>(utf8.data()), *end = i + utf8.size();
    while (i < end) {
        if ((*i & 0x80) == 0) {
            out.push_back(*i);
            ++i;
        } else if (i + 1 < end && (*i & 0xE0) == 0xC0) {
            out.push_back(((*i & 0x1f) << 6) | (i[1] & 0x3f));
            i += 2;
        } else if (i + 2 < end && (*i & 0xF0) == 0xE0) {
            out.push_back(((*i & 0x0f) << 12) | ((i[1] & 0x3f) << 6) | (i[2] & 0x3f));
            i += 3;
        } else if (i + 3 < end && (*i & 0xF8) == 0xF0) {
            out.push_back(((*i & 0x07) << 18) | ((i[1] & 0x3f) << 12) | ((i[2] & 0x3f) << 6) | (i[3] & 0x3f));
            i += 4;
        } else {
            out.push_back(InvalidCodepoint);
            ++i;
        }
    }
    return out;
}

std::string Utf8(uint64_t cp) {
    std::string out;
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xc0 | ((cp >> 6) & 0x1f));
        out += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000 || cp >= 0x110000) {
        if (cp >= 0x110000) cp = InvalidCodepoint;
        out += char(0xe0 | ((cp >> 12) & 0x0f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    } else {
        out += char(0xf0 | ((cp >> 18) & 0x07));
        out += char(0x80 | ((cp >> 12) & 0x3f));
        out += char(0x80 | ((cp >> 6) & 0x3f));
        out += char(0x80 | (cp & 0x3f));
    }
    return out;
}

uint64_t LineCount(const Text &doc) { return vec::Weight(doc) + 1; }
uint64_t LineStart(const Text &doc, uint64_t line) { return line ? vec::Seek(doc, line - 1) + 1 : 0; }
uint64_t LineEnd(const Text &doc, uint64_t line) { return line + 1 < LineCount(doc) ? vec::Seek(doc, line) : doc.Size; }
uint64_t LineLength(const Text &doc, uint64_t line) { return LineEnd(doc, line) - LineStart(doc, line); }
uint64_t Offset(const Text &doc, DocPos pos) { return LineStart(doc, pos.Line) + pos.Index; }
DocPos Pos(const Text &doc, uint64_t offset) {
    const auto line = vec::WeightBefore(doc, offset);
    return {line, offset - LineStart(doc, line)};
}
uint64_t CodepointAt(const Text &doc, DocPos pos) {
    const auto offset = Offset(doc, pos);
    return offset < LineEnd(doc, pos.Line) ? doc[offset] : InvalidCodepoint;
}
void LineCodepoints(const Text &doc, uint64_t line, std::vector<uint64_t> &out) {
    out.clear();
    for (auto i = LineStart(doc, line), end = LineEnd(doc, line); i < end; ++i) out.push_back(doc[i]);
}

Inserted InsertText(const Settings &config, const Text &doc, DocPos start, std::string_view text) {
    // `columns` counts glyph widths before the insert point on the current line, for tabs to spaces.
    std::vector<uint64_t> glyphs;
    uint64_t columns = 0;
    if (config.InsertSpacesOnTabs) {
        const auto lineStart = LineStart(doc, start.Line);
        for (uint64_t i = 0; i < start.Index; ++i) columns += GlyphWidth(doc[lineStart + i]);
    }
    uint64_t line = start.Line, index = start.Index;
    for (const auto cp : Codepoints(text)) {
        if (cp == Newline) {
            glyphs.push_back(Newline);
            ++line;
            index = 0;
            columns = 0;
        } else if (config.InsertSpacesOnTabs && cp == '\t') {
            const auto spaces = Columns(config.TabSize, cp, columns);
            glyphs.insert(glyphs.end(), spaces, ' ');
            index += spaces;
            columns += spaces;
        } else if (cp != '\r') {
            glyphs.push_back(cp);
            ++index;
            columns += GlyphWidth(cp);
        }
    }
    return {vec::Insert(doc, Offset(doc, start), Text{glyphs.begin(), glyphs.end()}), {line, index}};
}

Text DeleteText(const Text &doc, DocPos start, DocPos end) { return vec::Erase(doc, Offset(doc, start), Offset(doc, end)); }

std::string SectionText(const Text &doc, DocPos start, DocPos end) {
    std::string out;
    for (auto i = Offset(doc, start), last = Offset(doc, end); i < last; ++i) out += Utf8(doc[i]);
    return out;
}

std::string LineText(const Text &doc, uint64_t line) { return SectionText(doc, {line, 0}, {line, LineLength(doc, line)}); }

namespace {
// Returns whether `cp` continues the run that `first` starts.
// A run is whitespace, word glyphs or other glyphs, and only word glyphs with `wordOnly`.
bool SameRun(uint64_t first, uint64_t cp, bool wordOnly) {
    if (!wordOnly && IsWhiteSpace(first)) return IsWhiteSpace(cp);
    if (IsWord(first)) return IsWord(cp);
    return !wordOnly && !IsWord(cp) && !IsWhiteSpace(cp);
}

// Returns the position of the last non-whitespace glyph before `from` on its line, or the line start.
DocPos FindPreviousNonWhiteSpace(const Text &doc, DocPos from) {
    const auto lineStart = LineStart(doc, from.Line);
    auto index = from.Index;
    while (index > 0 && IsWhiteSpace(doc[lineStart + index - 1])) --index;
    return {from.Line, index > 0 ? index - 1 : 0};
}

// Returns the position of the first non-whitespace glyph at or after `from` on its line, or the line end.
DocPos FindNextNonWhiteSpace(const Text &doc, DocPos from) {
    const auto lineStart = LineStart(doc, from.Line), size = LineLength(doc, from.Line);
    auto index = from.Index;
    while (index < size && IsWhiteSpace(doc[lineStart + index])) ++index;
    return {from.Line, std::min(index, size)};
}
} // namespace

DocPos Left(const State &s, DocPos from, bool wordMode) {
    if (wordMode) return FindWordStart(s.Doc, FindPreviousNonWhiteSpace(s.Doc, Left(s, from)));
    if (from.Index == 0) return LeftBeforeHiddenLines(s, from);
    return {from.Line, from.Index - 1};
}

DocPos Right(const State &s, DocPos from, bool wordMode) {
    if (wordMode) return FindWordEnd(s.Doc, FindNextNonWhiteSpace(s.Doc, Right(s, from)));
    if (from.Index == LineLength(s.Doc, from.Line)) return RightAfterHiddenLines(s, from);
    return {from.Line, from.Index + 1};
}

DocPos Bottom(const Text &doc) {
    const auto last = LineCount(doc) - 1;
    return {last, LineLength(doc, last)};
}
DocPos StartOfLine(DocPos from) { return {from.Line, 0}; }
DocPos EndOfLine(const Text &doc, DocPos from) { return {from.Line, LineLength(doc, from.Line)}; }
DocPos NextLine(const State &s, DocPos from) { return Right(s, EndOfLine(s.Doc, from)); }

DocPos FindWordStart(const Text &doc, DocPos from, bool wordOnly) {
    if (from.Index == 0 || LineLength(doc, from.Line) == 0) return from;
    const auto lineStart = LineStart(doc, from.Line), first = doc[lineStart + from.Index - 1];
    auto index = from.Index;
    while (index > 0 && SameRun(first, doc[lineStart + index - 1], wordOnly)) --index;
    return {from.Line, index};
}

DocPos FindWordEnd(const Text &doc, DocPos from, bool wordOnly) {
    const auto lineStart = LineStart(doc, from.Line), size = LineLength(doc, from.Line);
    if (from.Index >= size) return from;
    const auto first = doc[lineStart + from.Index];
    auto index = from.Index;
    while (index < size && SameRun(first, doc[lineStart + index], wordOnly)) ++index;
    return {from.Line, index};
}

namespace {
bool IsWordStart(const Text &doc, DocPos pos, bool wordOnly) {
    if (pos.Index == LineLength(doc, pos.Line)) return false;
    const auto lineStart = LineStart(doc, pos.Line), next = doc[lineStart + pos.Index];
    if (wordOnly && !IsWord(next)) return false;
    return pos.Index == 0 || !SameRun(next, doc[lineStart + pos.Index - 1], wordOnly);
}

bool IsWordEnd(const Text &doc, DocPos pos, bool wordOnly) {
    if (pos.Index == 0) return false;
    const auto lineStart = LineStart(doc, pos.Line), previous = doc[lineStart + pos.Index - 1];
    if (wordOnly && !IsWord(previous)) return false;
    return pos.Index == LineLength(doc, pos.Line) || !SameRun(previous, doc[lineStart + pos.Index], wordOnly);
}

bool IsWholeWord(const Text &doc, DocPos start, DocPos end) {
    return start < end && start.Line == end.Line && IsWordStart(doc, start, false) && IsWordEnd(doc, end, false);
}
} // namespace

Selection WholeWord(const Text &doc, DocPos pos, bool wordOnly) {
    if (wordOnly && pos.Index != 0 && pos.Index != LineLength(doc, pos.Line) && !IsWord(CodepointAt(doc, pos))) return {pos, pos};
    if (IsWordStart(doc, pos, wordOnly)) return {pos, FindWordEnd(doc, pos, wordOnly)};
    if (IsWordEnd(doc, pos, wordOnly)) return {FindWordStart(doc, pos, wordOnly), pos};
    return {FindWordStart(doc, pos, wordOnly), FindWordEnd(doc, pos, wordOnly)};
}

std::optional<Selection> FindText(const Text &doc, DocPos from, std::string_view text, bool caseSensitive, bool wholeWord) {
    const auto lower = [&](uint64_t cp) { return caseSensitive ? cp : ToLower(cp); };
    auto search = Codepoints(text);
    for (auto &cp : search) cp = lower(cp);
    const auto first = Offset(doc, from);
    auto start = first;
    do {
        size_t j = 0;
        while (j < search.size() && start + j < doc.Size && lower(doc[start + j]) == search[j]) ++j;
        if (j == search.size()) {
            const Selection match{Pos(doc, start), Pos(doc, start + j)};
            if (!wholeWord || IsWholeWord(doc, match.Start, match.End)) return match;
        }
        start = start == doc.Size ? 0 : start + 1;
    } while (start != first);
    return {};
}

DocPos Normalize(const Text &doc, DocPos pos) {
    if (pos.Line >= LineCount(doc)) return Bottom(doc);
    if (pos.Index > LineLength(doc, pos.Line)) return EndOfLine(doc, pos);
    return pos;
}
uint64_t NormalizeLine(const Text &doc, uint64_t line) { return line >= LineCount(doc) ? LineCount(doc) - 1 : line; }
bool IsLastLine(const Text &doc, uint64_t line) { return line == LineCount(doc) - 1; }
} // namespace ipte
