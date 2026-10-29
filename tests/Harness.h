#pragma once

// Runs upstream's editor and ours in separate ImGui contexts with the same input and compares the
// draw data after every frame.

#include "Language.h"
#include "TextEdit.h"
#include "TextEditor.h"

#include <functional>
#include <string>
#include <variant>
#include <vector>

namespace harness {
struct Key {
    ImGuiKey Code;
    bool Down;
};
struct Char {
    unsigned Codepoint;
};
struct MousePos {
    float X, Y;
};
struct MouseButton {
    int Button;
    bool Down;
};
struct Wheel {
    float X, Y;
};
struct Focus {
    bool Focused;
};
using Event = std::variant<Key, Char, MousePos, MouseButton, Wheel, Focus>;
using Frame = std::vector<Event>;
using Script = std::vector<Frame>;

void Press(Script &s, ImGuiKey key, int mods = 0);
void Type(Script &s, std::string_view utf8);
void Click(Script &s, float x, float y, int button = 0, int mods = 0);
void Idle(Script &s, int frames);

struct MarkerSpec {
    uint64_t Line;
    ImU32 LineNumberColor, TextColor;
    std::string LineNumberTooltip, TextTooltip;
};
struct SquiggleSpec {
    ipte::DocPos Start, End;
    uint64_t Type;
    ImU32 Shade;
    std::string Tooltip;
    ipte::SquiggleStyle Style{ipte::SquiggleStyle::Wave};
};

struct Setup {
    std::string Text;
    ipte::Settings Config{};
    ImVec2 DisplaySize{800, 600};
    std::vector<MarkerSpec> Markers;
    std::vector<SquiggleSpec> Squiggles;
    float FontSize{0}; // Pushed around the editor when nonzero.
    float FontScaleDpi{1};
    float StyleAlpha{1};
    bool NavKeyboard{false};
    // Called on upstream after setup, for settings `Config` does not cover.
    std::function<void(TextEditor &)> Upstream{};
};

// Runs the script against both editors. Returns the first frame that differed, or -1. On a mismatch,
// writes both frames as PNGs under `IPTE_BINARY_DIR/mismatch/<name>` and describes the difference in `report`.
int Run(const std::string &name, const Setup &setup, const Script &script, std::string &report);
} // namespace harness
