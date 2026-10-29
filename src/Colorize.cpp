#include "Colorize.h"

#include "Document.h"
#include "Language.h"

#include <algorithm>
#include <ranges>

namespace ipte {
namespace {
enum struct LineState : uint8_t {
    InText,
    InComment,
    InCommentLevel1,
    InCommentLevel2,
    InCommentLevel3,
    InCommentLevel4,
    InCommentLevel5,
    InCommentLevel6,
    InCommentLevel7,
    InSingleQuotedString,
    InDoubleQuotedString,
    InOtherString,
    InOtherStringAlt,
    InStringLevel0,
    InStringLevel1,
    InStringLevel2,
    InStringLevel3,
    InStringLevel4,
    InStringLevel5,
    InStringLevel6,
    InStringLevel7,
};
constexpr size_t MaxCommentLevel = 7, MaxStringLevel = 7;

bool InComment(LineState s) { return s >= LineState::InComment && s <= LineState::InCommentLevel7; }
bool InStringLevel(LineState s) { return s >= LineState::InStringLevel0 && s <= LineState::InStringLevel7; }
LineState CommentLevel(size_t level) { return LineState(uint8_t(LineState::InComment) + level); }
LineState StringLevel(size_t level) { return LineState(uint8_t(LineState::InStringLevel0) + level); }

bool Matches(const uint64_t *glyph, const uint64_t *end, const std::string &text) {
    for (const auto cp : Codepoints(text)) {
        if (glyph == end || *glyph != cp) return false;
        ++glyph;
    }
    return true;
}

// Colors one line. Returns the state at its end.
LineState ColorizeLine(const Language &language, const uint64_t *begin, const uint64_t *end, uint8_t *colors, LineState state) {
    auto nonWhiteSpace = false;
    const auto *glyph = begin;
    const auto set = [&](const uint64_t *from, const uint64_t *to, Color c) {
        for (; from < to; ++from) colors[from - begin] = uint8_t(c);
    };
    const auto one = [&](Color c) { colors[glyph++ - begin] = uint8_t(c); };

    while (glyph < end) {
        const auto *const start = glyph;
        if (state == LineState::InText) {
            if (!nonWhiteSpace && language.Preprocess && *glyph != language.Preprocess && !IsWhiteSpace(*glyph)) nonWhiteSpace = true;

            if (language.CommentLevelStart) {
                size_t level;
                const auto *const tokenEnd = language.CommentLevelStart(glyph, end, level);
                if (tokenEnd != glyph) {
                    level = std::min(level, MaxCommentLevel);
                    state = CommentLevel(level);
                    set(glyph, tokenEnd, Color::Comment);
                    glyph = tokenEnd;
                }
            }
            if (glyph == start && language.StringLevelStart) {
                size_t level;
                const auto *const tokenEnd = language.StringLevelStart(glyph, end, level);
                if (tokenEnd != glyph) {
                    level = std::min(level, MaxStringLevel);
                    state = StringLevel(level);
                    set(glyph, tokenEnd, Color::String);
                    glyph = tokenEnd;
                }
            }
            if (glyph == start) {
                if (IsWhiteSpace(*glyph)) {
                    one(Color::Whitespace);
                } else if (!language.CommentStart.empty() && Matches(glyph, end, language.CommentStart)) {
                    state = LineState::InComment;
                    const auto size = language.CommentStart.size();
                    set(glyph, glyph + size, Color::Comment);
                    glyph += size;
                } else if (!language.SingleLineComment.empty() && Matches(glyph, end, language.SingleLineComment)) {
                    set(glyph, end, Color::Comment);
                    glyph = end;
                } else if (!language.SingleLineCommentAlt.empty() && Matches(glyph, end, language.SingleLineCommentAlt)) {
                    set(glyph, end, Color::Comment);
                    glyph = end;
                } else if (!language.OtherStringStart.empty() && Matches(glyph, end, language.OtherStringStart)) {
                    state = LineState::InOtherString;
                    const auto size = language.OtherStringStart.size();
                    set(glyph, glyph + size, Color::String);
                    glyph += size;
                } else if (!language.OtherStringAltStart.empty() && Matches(glyph, end, language.OtherStringAltStart)) {
                    state = LineState::InOtherStringAlt;
                    const auto size = language.OtherStringAltStart.size();
                    set(glyph, glyph + size, Color::String);
                    glyph += size;
                } else if (language.HasSingleQuotedStrings && *glyph == '\'') {
                    state = LineState::InSingleQuotedString;
                    one(Color::String);
                } else if (language.HasDoubleQuotedStrings && *glyph == '"') {
                    state = LineState::InDoubleQuotedString;
                    one(Color::String);
                } else if (language.Preprocess && !nonWhiteSpace && *glyph == language.Preprocess) {
                    set(begin, end, Color::Preprocessor);
                    glyph = end;
                }
            }
            if (glyph == start) {
                Color color;
                const uint64_t *tokenEnd;
                if (language.CustomTokenizer && (tokenEnd = language.CustomTokenizer(glyph, end, color)) != glyph) {
                    set(glyph, tokenEnd, color);
                    glyph = tokenEnd;
                } else if (language.GetIdentifier && (tokenEnd = language.GetIdentifier(glyph, end)) != glyph) {
                    std::string identifier;
                    color = Color::Identifier;
                    for (const auto *i = glyph; i < tokenEnd; ++i) identifier += Utf8(language.CaseSensitive ? *i : ToLower(*i));
                    if (language.Keywords.contains(identifier)) color = Color::Keyword;
                    else if (language.Declarations.contains(identifier)) color = Color::Declaration;
                    else if (language.Identifiers.contains(identifier)) color = Color::KnownIdentifier;
                    set(glyph, tokenEnd, color);
                    glyph = tokenEnd;
                } else if (language.GetNumber && (tokenEnd = language.GetNumber(glyph, end)) != glyph) {
                    set(glyph, tokenEnd, Color::Number);
                    glyph = tokenEnd;
                } else if (language.IsPunctuation && language.IsPunctuation(*glyph)) {
                    one(Color::Punctuation);
                } else {
                    one(Color::Text);
                }
            }
        } else if (InComment(state)) {
            auto const size = language.CommentEnd.size();
            if (size && Matches(glyph, end, language.CommentEnd)) {
                set(glyph, glyph + size, Color::Comment);
                glyph += size;
                state = LineState::InText;
            } else if (language.CommentLevelEnd) {
                size_t level;
                const auto *const tokenEnd = language.CommentLevelEnd(glyph, end, level);
                if (tokenEnd != glyph) {
                    level = std::min(level, MaxCommentLevel);
                    if (state == CommentLevel(level)) {
                        set(glyph, tokenEnd, Color::Comment);
                        glyph = tokenEnd;
                        state = LineState::InText;
                    }
                }
            }
            if (InComment(state)) one(Color::Comment);
        } else if (InStringLevel(state)) {
            size_t level;
            const auto *const tokenEnd = language.StringLevelEnd(glyph, end, level);
            if (tokenEnd != glyph) {
                level = std::min(level, MaxStringLevel);
                if (state == StringLevel(level)) {
                    set(glyph, tokenEnd, Color::String);
                    glyph = tokenEnd;
                    state = LineState::InText;
                } else {
                    one(Color::String);
                }
            } else {
                one(Color::String);
            }
        } else if (state == LineState::InOtherString || state == LineState::InOtherStringAlt) {
            const auto &closer = state == LineState::InOtherString ? language.OtherStringEnd : language.OtherStringAltEnd;
            if (*glyph == language.StringEscape) {
                one(Color::String);
                if (glyph < end) one(Color::String);
            } else if (Matches(glyph, end, closer)) {
                const auto size = closer.size();
                set(glyph, glyph + size, Color::String);
                glyph += size;
                state = LineState::InText;
            } else {
                one(Color::String);
            }
        } else {
            const uint64_t quote = state == LineState::InSingleQuotedString ? '\'' : '"';
            if (*glyph == language.StringEscape) {
                one(Color::String);
                if (glyph < end) one(Color::String);
            } else if (*glyph == quote) {
                one(Color::String);
                state = LineState::InText;
            } else {
                one(Color::String);
            }
        }
    }
    return state;
}

// Pairs the brackets colored as punctuation. Colors them by depth, unmatched ones as errors.
void MatchBrackets(const State &s, View &v) {
    static const Color bracketColors[]{Color::MatchingBracketLevel1, Color::MatchingBracketLevel2, Color::MatchingBracketLevel3};
    std::vector<size_t> levels;
    uint64_t level = 0;
    const auto lines = LineCount(s.Doc);
    for (uint64_t line = 0; line < lines; ++line) {
        const auto start = LineStart(s.Doc, line), size = LineLength(s.Doc, line);
        for (uint64_t index = 0; index < size; ++index) {
            const auto offset = start + index;
            const auto cp = s.Doc[offset];
            auto &color = v.Colors[offset];
            const auto candidate = color == uint8_t(Color::Punctuation);
            if (candidate && IsBracketOpener(cp)) {
                levels.push_back(v.Brackets.size());
                v.Brackets.push_back({cp, {line, index}, 0, {}, level, true});
                color = uint8_t(bracketColors[level % 3]);
                ++level;
            } else if (candidate && IsBracketCloser(cp)) {
                if (!levels.empty()) {
                    auto &last = v.Brackets[levels.back()];
                    levels.pop_back();
                    --level;
                    if (last.StartChar == ToPairOpener(cp)) {
                        color = uint8_t(bracketColors[level % 3]);
                        last.EndChar = cp;
                        last.End = {line, index};
                    } else {
                        color = uint8_t(Color::MatchingBracketError);
                        v.Colors[Offset(s.Doc, last.Start)] = uint8_t(Color::MatchingBracketError);
                        v.Brackets.pop_back();
                    }
                } else {
                    color = uint8_t(Color::MatchingBracketError);
                }
            }
        }
    }
    for (const auto level : std::views::reverse(levels)) {
        v.Colors[Offset(s.Doc, v.Brackets[level].Start)] = uint8_t(Color::MatchingBracketError);
        v.Brackets.erase(v.Brackets.begin() + int64_t(level));
    }

    if (s.Config.Language && s.Config.Language->IndentationForBlocks) {
        for (uint64_t i = 0; i < lines; ++i) {
            if (!LineLength(s.Doc, i)) continue;
            const auto currentIndent = LineIndent(s, i);
            auto endLine = i;
            for (uint64_t j = i + 1; j < lines; ++j) {
                if (!LineLength(s.Doc, j)) continue;
                if (LineIndent(s, j) > currentIndent) endLine = j;
                else break;
            }
            if (endLine > i) v.Brackets.push_back({0, {i, 0}, 0, {endLine, LineLength(s.Doc, endLine)}, 0, false});
        }
        std::ranges::sort(v.Brackets, [](const Bracket &a, const Bracket &b) { return a.Start < b.Start; });
    }
}
} // namespace

uint64_t LineIndent(const State &s, uint64_t line) {
    uint64_t indent = 0;
    for (auto i = LineStart(s.Doc, line), end = LineEnd(s.Doc, line); i < end; ++i) {
        const auto cp = s.Doc[i];
        if (cp == ' ') ++indent;
        else if (cp == '\t') indent += s.Config.TabSize - (indent % s.Config.TabSize);
        else break;
    }
    return indent;
}

bool Refresh(State &s) {
    auto &v = s.Derived;
    if (v.Root == s.Doc.Root && v.Tail == s.Doc.Tail && v.Size == s.Doc.Size && v.Lang == s.Config.Language && v.ShowMatchingBrackets == s.Config.ShowMatchingBrackets && v.TabSize == s.Config.TabSize) return false;
    const auto previous = std::move(v.Brackets);
    v.Root = s.Doc.Root;
    v.Tail = s.Doc.Tail;
    v.Size = s.Doc.Size;
    v.Lang = s.Config.Language;
    v.ShowMatchingBrackets = s.Config.ShowMatchingBrackets;
    v.TabSize = s.Config.TabSize;
    v.Colors.assign(s.Doc.Size, uint8_t(Color::Text));
    v.Brackets.clear();
    ++v.Version;
    const auto lines = LineCount(s.Doc);
    v.LineStates.assign(lines, uint8_t(LineState::InText));
    if (!s.Config.Language) return previous != v.Brackets;

    std::vector<uint64_t> line;
    auto state = LineState::InText;
    for (uint64_t i = 0; i < lines; ++i) {
        v.LineStates[i] = uint8_t(state);
        const auto start = LineStart(s.Doc, i), end = LineEnd(s.Doc, i);
        line.assign(end - start, 0);
        for (auto k = start; k < end; ++k) line[k - start] = s.Doc[k];
        state = ColorizeLine(*s.Config.Language, line.data(), line.data() + line.size(), v.Colors.data() + start, state);
    }
    if (s.Config.ShowMatchingBrackets) MatchBrackets(s, v);
    return previous != v.Brackets;
}

Color ColorAt(const State &s, uint64_t offset) { return Color(s.Derived.Colors[offset]); }
bool LineInComment(const State &s, uint64_t line) { return line < s.Derived.LineStates.size() && InComment(LineState(s.Derived.LineStates[line])); }
bool LineInString(const State &s, uint64_t line) {
    if (line >= s.Derived.LineStates.size()) return false;
    const auto state = LineState(s.Derived.LineStates[line]);
    return state >= LineState::InSingleQuotedString && state <= LineState::InStringLevel7;
}

const Bracket *EnclosingBrackets(const View &v, DocPos pos) {
    const Bracket *found = nullptr;
    for (const auto &b : v.Brackets) {
        if (b.IsAfter(pos)) break;
        if (b.IsAround(pos)) found = &b;
    }
    return found;
}

const Bracket *EnclosingBrackets(const View &v, DocPos first, DocPos last) {
    const Bracket *found = nullptr;
    for (const auto &b : v.Brackets) {
        if (b.IsAfter(first)) break;
        if (b.IsAround(first) && b.IsAround(last)) found = &b;
    }
    return found;
}

const Bracket *InnerBrackets(const View &v, DocPos first, DocPos last) {
    const auto *outer = EnclosingBrackets(v, first, last);
    if (!outer) return nullptr;
    for (const auto *i = outer + 1; i < v.Brackets.data() + v.Brackets.size(); ++i) {
        if (i->Level <= outer->Level) return nullptr;
        if (i->Level == outer->Level + 1 && i->Start > first && i->End < last) return i;
    }
    return nullptr;
}
} // namespace ipte
