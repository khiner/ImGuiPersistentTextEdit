#include "Document.h"

#include "Layout.h"

namespace ipte {
bool IsPairOpener(uint64_t cp) { return cp == '{' || cp == '[' || cp == '(' || cp == '\'' || cp == '"'; }
bool IsPairCloser(uint64_t cp) { return cp == '}' || cp == ']' || cp == ')' || cp == '\'' || cp == '"'; }
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

std::string Utf8(const std::vector<uint64_t> &codepoints) {
    std::string out;
    for (const auto cp : codepoints) out += Utf8(cp);
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

uint64_t SpacesToTab(const Settings &config, const Text &doc, DocPos pos) {
    uint64_t columns = 0;
    const auto start = LineStart(doc, pos.Line);
    for (uint64_t i = 0; i < pos.Index; ++i) columns += GlyphWidth(doc[start + i]);
    return ((columns / config.TabSize) + 1) * config.TabSize - columns;
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
            const auto spaces = ((columns / config.TabSize) + 1) * config.TabSize - columns;
            for (uint64_t k = 0; k < spaces; ++k) glyphs.push_back(' ');
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

DocPos Left(const State &s, DocPos from, bool wordMode) {
    if (wordMode) return FindWordStart(s.Doc, FindPreviousNonWhiteSpace(s, Left(s, from), false));
    if (from.Index == 0) return LeftBeforeHiddenLines(s, from);
    return {from.Line, from.Index - 1};
}

DocPos Right(const State &s, DocPos from, bool wordMode) {
    if (wordMode) return FindWordEnd(s.Doc, FindNextNonWhiteSpace(s, Right(s, from), false));
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
    const auto lineStart = LineStart(doc, from.Line), lineSize = LineLength(doc, from.Line);
    if (from.Index == 0 || lineSize == 0) return from;
    auto index = from.Index;
    const auto at = [&](uint64_t i) { return doc[lineStart + i]; };
    const auto first = at(index - 1);
    if (!wordOnly && IsWhiteSpace(first)) {
        while (index > 0 && IsWhiteSpace(at(index - 1))) --index;
    } else if (IsWord(first)) {
        while (index > 0 && IsWord(at(index - 1))) --index;
    } else {
        while (!wordOnly && index > 0 && !IsWord(at(index - 1)) && !IsWhiteSpace(at(index - 1))) --index;
    }
    return {from.Line, index};
}

DocPos FindWordEnd(const Text &doc, DocPos from, bool wordOnly) {
    const auto lineStart = LineStart(doc, from.Line), size = LineLength(doc, from.Line);
    auto index = from.Index;
    if (index >= size) return from;
    const auto at = [&](uint64_t i) { return doc[lineStart + i]; };
    const auto first = at(index);
    if (!wordOnly && IsWhiteSpace(first)) {
        while (index < size && IsWhiteSpace(at(index))) ++index;
    } else if (IsWord(first)) {
        while (index < size && IsWord(at(index))) ++index;
    } else {
        while (!wordOnly && index < size && !IsWord(at(index)) && !IsWhiteSpace(at(index))) ++index;
    }
    return {from.Line, index};
}

DocPos FindPreviousNonWhiteSpace(const State &s, DocPos from, bool includeEndOfLine) {
    const auto &doc = s.Doc;
    while (true) {
        const auto lineStart = LineStart(doc, from.Line);
        auto index = from.Index;
        while (index > 0) {
            --index;
            if (!IsWhiteSpace(doc[lineStart + index])) return {from.Line, index};
        }
        if (from.Line == 0 || !includeEndOfLine) return {from.Line, 0};
        from = LeftBeforeHiddenLines(s, from);
    }
}

DocPos FindNextNonWhiteSpace(const State &s, DocPos from, bool includeEndOfLine) {
    const auto &doc = s.Doc;
    while (true) {
        const auto lineStart = LineStart(doc, from.Line), size = LineLength(doc, from.Line);
        auto index = from.Index;
        while (index < size) {
            if (!IsWhiteSpace(doc[lineStart + index])) return {from.Line, index};
            ++index;
        }
        // Upstream compares against the line count, which never matches. See UpstreamBugs.md.
        if (from.Line == LineCount(doc) || !includeEndOfLine) return {from.Line, size};
        const auto next = RightAfterHiddenLines(s, from);
        if (next == from) return next;
        from = next;
    }
}

bool IsWordStart(const Text &doc, DocPos pos, bool wordOnly) {
    const auto size = LineLength(doc, pos.Line);
    if (pos.Index == size) return false;
    const auto lineStart = LineStart(doc, pos.Line);
    if (pos.Index == 0) return size != 0 && (!wordOnly || IsWord(doc[lineStart]));
    const auto g1 = doc[lineStart + pos.Index - 1], g2 = doc[lineStart + pos.Index];
    if (wordOnly) return !IsWord(g1) && IsWord(g2);
    if (IsWord(g2)) return !IsWord(g1);
    if (IsWhiteSpace(g2)) return !IsWhiteSpace(g1);
    return IsWord(g1) || IsWhiteSpace(g1);
}

bool IsWordEnd(const Text &doc, DocPos pos, bool wordOnly) {
    const auto size = LineLength(doc, pos.Line);
    if (pos.Index == 0) return false;
    const auto lineStart = LineStart(doc, pos.Line);
    if (pos.Index == size) return size != 0 && (!wordOnly || IsWord(doc[lineStart + size - 1]));
    const auto g1 = doc[lineStart + pos.Index - 1], g2 = doc[lineStart + pos.Index];
    if (wordOnly) return IsWord(g1) && !IsWord(g2);
    if (IsWord(g1)) return !IsWord(g2);
    if (IsWhiteSpace(g1)) return !IsWhiteSpace(g2);
    return IsWord(g2) || IsWhiteSpace(g2);
}

bool IsWholeWord(const Text &doc, DocPos start, DocPos end, bool wordOnly) {
    if (end <= start || start.Line != end.Line) return false;
    return IsWordStart(doc, start, wordOnly) && IsWordEnd(doc, end, wordOnly);
}

Selection WholeWord(const Text &doc, DocPos pos, bool wordOnly) {
    const auto size = LineLength(doc, pos.Line);
    if (wordOnly && pos.Index != 0 && pos.Index != size && !IsWord(CodepointAt(doc, pos))) return {pos, pos};
    if (IsWordStart(doc, pos, wordOnly)) return {pos, FindWordEnd(doc, pos, wordOnly)};
    if (IsWordEnd(doc, pos, wordOnly)) return {FindWordStart(doc, pos, wordOnly), pos};
    return {FindWordStart(doc, pos, wordOnly), FindWordEnd(doc, pos, wordOnly)};
}

bool FindText(const Text &doc, DocPos from, std::string_view text, bool caseSensitive, bool wholeWord, DocPos &start, DocPos &end) {
    std::vector<uint64_t> search;
    for (const auto cp : Codepoints(text)) search.push_back(caseSensitive ? cp : ToLower(cp));
    const auto lines = LineCount(doc);
    const auto startLine = from.Line, startIndex = from.Index;
    auto searchLine = startLine, searchIndex = startIndex;
    do {
        auto line = searchLine, index = searchIndex;
        auto lineStart = LineStart(doc, line), lineSize = LineLength(doc, line);
        bool done = false;
        size_t j = 0;
        while (!done && j < search.size()) {
            if (search[j] == Newline) {
                if (index == lineSize) {
                    if (line == lines - 1) {
                        done = true;
                    } else {
                        ++line;
                        index = 0;
                        lineStart = LineStart(doc, line);
                        lineSize = LineLength(doc, line);
                        ++j;
                    }
                } else {
                    done = true;
                }
            } else if (index == lineSize) {
                done = true;
            } else {
                auto ch = doc[lineStart + index];
                if (!caseSensitive) ch = ToLower(ch);
                if (ch == search[j]) {
                    ++index;
                    ++j;
                } else {
                    done = true;
                }
            }
        }
        if (j == search.size()) {
            start = {searchLine, searchIndex};
            end = {line, index};
            if (!wholeWord || IsWholeWord(doc, start, end)) return true;
        }
        if (searchIndex == LineLength(doc, searchLine)) {
            searchLine = searchLine == lines - 1 ? 0 : searchLine + 1;
            searchIndex = 0;
        } else {
            ++searchIndex;
        }
    } while (searchLine != startLine || searchIndex != startIndex);
    return false;
}

DocPos Normalize(const Text &doc, DocPos pos) {
    if (pos.Line >= LineCount(doc)) return Bottom(doc);
    if (pos.Index > LineLength(doc, pos.Line)) return EndOfLine(doc, pos);
    return pos;
}
uint64_t NormalizeLine(const Text &doc, uint64_t line) { return line >= LineCount(doc) ? LineCount(doc) - 1 : line; }
bool IsLastLine(const Text &doc, uint64_t line) { return line == LineCount(doc) - 1; }
} // namespace ipte
