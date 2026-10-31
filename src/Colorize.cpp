#include "Colorize.h"

#include "Document.h"
#include "Language.h"
#include "Layout.h"

#include <algorithm>
#include <ranges>
#include <tuple>

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
constexpr size_t MaxLevel = 7;

bool InComment(LineState s) { return s >= LineState::InComment && s <= LineState::InCommentLevel7; }
bool InStringLevel(LineState s) { return s >= LineState::InStringLevel0 && s <= LineState::InStringLevel7; }

bool Matches(const uint64_t *glyph, const uint64_t *end, const std::string &text) { return std::ranges::starts_with(std::ranges::subrange(glyph, end), Codepoints(text)); }

// Colors one line. Returns the state at its end.
LineState ColorizeLine(const Language &language, const uint64_t *begin, const uint64_t *end, uint8_t *colors, LineState state) {
    auto nonWhiteSpace = false;
    const auto *glyph = begin;
    // Colors the glyphs up to `to` and moves past them.
    const auto take = [&](const uint64_t *to, Color c) {
        for (; glyph < to; ++glyph) colors[glyph - begin] = uint8_t(c);
    };
    const auto one = [&](Color c) { take(glyph + 1, c); };
    const auto starts = [&](const std::string &token) { return !token.empty() && Matches(glyph, end, token); };
    // Takes a leveled token. In text it enters the token's level. In a level it leaves it if the token's level matches.
    const auto takeLevel = [&](Language::LevelTokenizer tokenize, LineState level0, Color c) {
        size_t level;
        const auto *const tokenEnd = tokenize(glyph, end, level);
        if (tokenEnd == glyph) return;
        const auto found = LineState(uint8_t(level0) + std::min(level, MaxLevel));
        if (state == LineState::InText) state = found;
        else if (state == found) state = LineState::InText;
        else return;
        take(tokenEnd, c);
    };

    while (glyph < end) {
        const auto *const start = glyph;
        if (state == LineState::InText) {
            if (!nonWhiteSpace && language.Preprocess && *glyph != language.Preprocess && !IsWhiteSpace(*glyph)) nonWhiteSpace = true;

            if (language.CommentLevelStart) takeLevel(language.CommentLevelStart, LineState::InComment, Color::Comment);
            if (glyph == start && language.StringLevelStart) takeLevel(language.StringLevelStart, LineState::InStringLevel0, Color::String);
            if (glyph == start) {
                if (IsWhiteSpace(*glyph)) {
                    one(Color::Whitespace);
                } else if (starts(language.CommentStart)) {
                    state = LineState::InComment;
                    take(glyph + language.CommentStart.size(), Color::Comment);
                } else if (starts(language.SingleLineComment) || starts(language.SingleLineCommentAlt)) {
                    take(end, Color::Comment);
                } else if (starts(language.OtherStringStart)) {
                    state = LineState::InOtherString;
                    take(glyph + language.OtherStringStart.size(), Color::String);
                } else if (starts(language.OtherStringAltStart)) {
                    state = LineState::InOtherStringAlt;
                    take(glyph + language.OtherStringAltStart.size(), Color::String);
                } else if (language.HasSingleQuotedStrings && *glyph == '\'') {
                    state = LineState::InSingleQuotedString;
                    one(Color::String);
                } else if (language.HasDoubleQuotedStrings && *glyph == '"') {
                    state = LineState::InDoubleQuotedString;
                    one(Color::String);
                } else if (language.Preprocess && !nonWhiteSpace && *glyph == language.Preprocess) {
                    glyph = begin;
                    take(end, Color::Preprocessor);
                }
            }
            if (glyph == start) {
                Color color;
                const uint64_t *tokenEnd;
                if (language.CustomTokenizer && (tokenEnd = language.CustomTokenizer(glyph, end, color)) != glyph) {
                    take(tokenEnd, color);
                } else if (language.GetIdentifier && (tokenEnd = language.GetIdentifier(glyph, end)) != glyph) {
                    std::string identifier;
                    color = Color::Identifier;
                    for (const auto *i = glyph; i < tokenEnd; ++i) identifier += Utf8(language.CaseSensitive ? *i : ToLower(*i));
                    if (language.Keywords.contains(identifier)) color = Color::Keyword;
                    else if (language.Declarations.contains(identifier)) color = Color::Declaration;
                    else if (language.Identifiers.contains(identifier)) color = Color::KnownIdentifier;
                    take(tokenEnd, color);
                } else if (language.GetNumber && (tokenEnd = language.GetNumber(glyph, end)) != glyph) {
                    take(tokenEnd, Color::Number);
                } else {
                    one(language.IsPunctuation && language.IsPunctuation(*glyph) ? Color::Punctuation : Color::Text);
                }
            }
        } else if (InComment(state)) {
            if (starts(language.CommentEnd)) {
                state = LineState::InText;
                take(glyph + language.CommentEnd.size(), Color::Comment);
            } else if (language.CommentLevelEnd) {
                takeLevel(language.CommentLevelEnd, LineState::InComment, Color::Comment);
            }
            if (InComment(state)) one(Color::Comment);
        } else if (InStringLevel(state)) {
            takeLevel(language.StringLevelEnd, LineState::InStringLevel0, Color::String);
            if (InStringLevel(state)) one(Color::String);
        } else if (*glyph == language.StringEscape) {
            one(Color::String);
            if (glyph < end) one(Color::String);
        } else if (state == LineState::InOtherString || state == LineState::InOtherStringAlt) {
            const auto &closer = state == LineState::InOtherString ? language.OtherStringEnd : language.OtherStringAltEnd;
            if (Matches(glyph, end, closer)) {
                state = LineState::InText;
                take(glyph + closer.size(), Color::String);
            } else {
                one(Color::String);
            }
        } else {
            if (*glyph == (state == LineState::InSingleQuotedString ? '\'' : '"')) state = LineState::InText;
            one(Color::String);
        }
    }
    return state;
}

// Leading whitespace in columns.
uint64_t LineIndent(const State &s, uint64_t line) {
    uint64_t indent = 0;
    for (auto i = LineStart(s.Doc, line), end = LineEnd(s.Doc, line); i < end; ++i) {
        const auto cp = s.Doc[i];
        if (cp != ' ' && cp != '\t') break;
        indent += Columns(s.Config.TabSize, cp, indent);
    }
    return indent;
}

// Pairs the brackets colored as punctuation. Colors them by depth, unmatched ones as errors.
void MatchBrackets(const State &s, View &v) {
    static const Color bracketColors[]{Color::MatchingBracketLevel1, Color::MatchingBracketLevel2, Color::MatchingBracketLevel3};
    std::vector<size_t> levels;
    const auto lines = LineCount(s.Doc);
    for (uint64_t line = 0; line < lines; ++line) {
        const auto start = LineStart(s.Doc, line), size = LineLength(s.Doc, line);
        for (uint64_t index = 0; index < size; ++index) {
            const auto offset = start + index;
            auto &color = v.Colors[offset];
            if (color != uint8_t(Color::Punctuation)) continue;
            const auto cp = s.Doc[offset];
            if (IsBracketOpener(cp)) {
                v.Brackets.push_back({cp, {line, index}, {}, levels.size(), true});
                color = uint8_t(bracketColors[levels.size() % 3]);
                levels.push_back(v.Brackets.size() - 1);
            } else if (IsBracketCloser(cp)) {
                if (!levels.empty()) {
                    auto &last = v.Brackets[levels.back()];
                    levels.pop_back();
                    if (last.StartChar == ToPairOpener(cp)) {
                        color = uint8_t(bracketColors[levels.size() % 3]);
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
            if (endLine > i) v.Brackets.push_back({0, {i, 0}, {endLine, LineLength(s.Doc, endLine)}, 0, false});
        }
        std::ranges::sort(v.Brackets, {}, &Bracket::Start);
    }
}

LineState LineStateAt(const State &s, uint64_t line) { return line < s.Derived.LineStates.size() ? LineState(s.Derived.LineStates[line]) : LineState::InText; }
} // namespace

bool Refresh(State &s) {
    auto &v = s.Derived;
    auto key = std::tie(v.Root, v.Tail, v.Size, v.Lang, v.ShowMatchingBrackets, v.TabSize);
    const auto current = std::tie(s.Doc.Root, s.Doc.Tail, s.Doc.Size, s.Config.Language, s.Config.ShowMatchingBrackets, s.Config.TabSize);
    if (key == current) return false;
    key = current;
    const auto previous = std::move(v.Brackets);
    v.Colors.assign(s.Doc.Size, uint8_t(Color::Text));
    v.Brackets.clear();
    ++v.Version;
    const auto lines = LineCount(s.Doc);
    v.LineStates.assign(lines, uint8_t(LineState::InText));
    if (s.Config.Language) {
        std::vector<uint64_t> line;
        auto state = LineState::InText;
        for (uint64_t i = 0; i < lines; ++i) {
            v.LineStates[i] = uint8_t(state);
            LineCodepoints(s.Doc, i, line);
            state = ColorizeLine(*s.Config.Language, line.data(), line.data() + line.size(), v.Colors.data() + LineStart(s.Doc, i), state);
        }
        if (s.Config.ShowMatchingBrackets) MatchBrackets(s, v);
    }
    return previous != v.Brackets;
}

Color ColorAt(const State &s, uint64_t offset) { return Color(s.Derived.Colors[offset]); }
bool LineInComment(const State &s, uint64_t line) { return InComment(LineStateAt(s, line)); }
bool LineInString(const State &s, uint64_t line) { return LineStateAt(s, line) >= LineState::InSingleQuotedString; }

const Bracket *EnclosingBrackets(const View &v, DocPos pos) { return EnclosingBrackets(v, pos, pos); }

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
