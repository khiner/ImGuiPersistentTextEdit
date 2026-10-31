#pragma once

#include "TextEdit.h"
#include "Unicode.h"

#include <optional>

namespace ipte {
constexpr uint64_t Newline = '\n';
constexpr uint64_t InvalidCodepoint = 0xFFFD;

bool IsPairOpener(uint64_t cp);
bool IsBracketOpener(uint64_t cp);
bool IsBracketCloser(uint64_t cp);
uint64_t ToPairCloser(uint64_t cp);
uint64_t ToPairOpener(uint64_t cp);

// UTF-8 conversion, matching upstream.
std::vector<uint64_t> Codepoints(std::string_view utf8);
std::string Utf8(uint64_t codepoint);

uint64_t LineCount(const Text &doc);
uint64_t LineStart(const Text &doc, uint64_t line);
uint64_t LineEnd(const Text &doc, uint64_t line);
uint64_t LineLength(const Text &doc, uint64_t line);
uint64_t Offset(const Text &doc, DocPos pos);
DocPos Pos(const Text &doc, uint64_t offset);
// The codepoint at `pos`, or `InvalidCodepoint` at the end of a line.
uint64_t CodepointAt(const Text &doc, DocPos pos);
// Sets `out` to the codepoints of `line`.
void LineCodepoints(const Text &doc, uint64_t line, std::vector<uint64_t> &out);

struct Inserted {
    Text Doc;
    DocPos End;
};
Inserted InsertText(const Settings &config, const Text &doc, DocPos start, std::string_view text);
Text DeleteText(const Text &doc, DocPos start, DocPos end);
std::string SectionText(const Text &doc, DocPos start, DocPos end);
std::string LineText(const Text &doc, uint64_t line);

// These skip hidden lines, so they take the state.
DocPos Left(const State &s, DocPos from, bool wordMode = false);
DocPos Right(const State &s, DocPos from, bool wordMode = false);
DocPos Bottom(const Text &doc);
DocPos StartOfLine(DocPos from);
DocPos EndOfLine(const Text &doc, DocPos from);
DocPos NextLine(const State &s, DocPos from);
DocPos FindWordStart(const Text &doc, DocPos from, bool wordOnly = false);
DocPos FindWordEnd(const Text &doc, DocPos from, bool wordOnly = false);
struct Selection {
    DocPos Start, End;
};
Selection WholeWord(const Text &doc, DocPos pos, bool wordOnly = false);
// Finds `text` at or after `from`, wrapping around.
std::optional<Selection> FindText(const Text &doc, DocPos from, std::string_view text, bool caseSensitive, bool wholeWord);
DocPos Normalize(const Text &doc, DocPos pos);
uint64_t NormalizeLine(const Text &doc, uint64_t line);
bool IsLastLine(const Text &doc, uint64_t line);
} // namespace ipte
