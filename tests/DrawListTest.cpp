// Compares the draw data of upstream's editor and ours over scripted interactions.
//   cmake --build build --target DrawListTest && ./build/tests/DrawListTest

#include "Harness.h"

#include <boost/ut.hpp>

using namespace boost::ut;
using namespace harness;

namespace {
// Expects the first frame that differs from upstream to be `frame`, -1 for none.
void Matches(const std::string &name, const Setup &setup, const Script &script, int frame = -1) {
    std::string report;
    expect(Run(name, setup, script, report) == frame) << name << ": " << report << " (see " IPTE_BINARY_DIR "/mismatch/" << name << ")";
}

// ImGui maps ImGuiMod_Ctrl in shortcuts to Cmd on macOS.
constexpr auto Ctrl = ImGuiMod_Super;

const std::string Sample = "int main() {\n\treturn 0;\n}\n\nhello world, this is a line\n\tindented\n";

const std::string Code = R"(#include <stdio.h>

/* a comment
   over lines */
int main(int argc, char **argv) {
	// single line
	const char *s = "a \"string\" (with brackets]";
	float x = 1.5e3f + 0x1F;
	if (argc > 1) { printf("%s\n", argv[1]); }
	return (argc * 2;
}
)";

const std::string PythonCode = R"(def f(x):
    """doc"""
    if x:
        return [x, (x + 1)]
    else:
        return None

class A:
    pass
)";

std::string Long() {
    std::string text;
    for (int i = 0; i < 300; ++i) text += "line " + std::to_string(i) + (i % 7 == 0 ? std::string(120, 'x') : std::string()) + "\n";
    return text;
}

std::vector<std::string> Suggest(const ipte::AutoCompleteContext &context) {
    static const std::string words[]{"argc", "argv", "include", "int", "printf", "render", "replace", "result", "return"};
    std::vector<std::string> out;
    for (const auto &w : words)
        if (std::ranges::starts_with(w, context.SearchTerm, {}, {}, [](char c) { return std::tolower(c); })) out.push_back(w);
    return out;
}

Script Focused() {
    Script s;
    Idle(s, 2);
    Click(s, 300, 100);
    Idle(s, 2);
    return s;
}
} // namespace

int main() {
    "empty document"_test = [] {
        Script script;
        Idle(script, 3);
        Matches("empty", {.Text = ""}, script);
    };

    "harness sees a difference"_test = [] {
        Script script;
        Idle(script, 2);
        Matches("negative", {.Text = "\tx", .Upstream = [](TextEditor &e) { e.SetTabSize(2); }}, script, 0);
    };

    "typing reaches the document"_test = [] {
        auto script = Focused();
        Type(script, "z");
        Matches("negative-typing", {.Text = Sample, .Upstream = [](TextEditor &e) { e.SetReadOnlyEnabled(true); }}, script, int(script.size()) - 1);
    };

    "plain text"_test = [] {
        Script script;
        Idle(script, 3);
        Matches("plain", {.Text = "hello world\n\tsecond line\n"}, script);
    };

    "click focuses and places the caret"_test = [] {
        auto script = Focused();
        Idle(script, 60); // Longer than one caret blink.
        Matches("click", {.Text = Sample}, script);
    };

    "typing"_test = [] {
        auto script = Focused();
        Type(script, "abc def\n\tghi (x) {\n}");
        Idle(script, 3);
        Matches("typing", {.Text = Sample}, script);
    };

    "movement and selection"_test = [] {
        auto script = Focused();
        Press(script, {ImGuiKey_RightArrow, ImGuiKey_RightArrow, ImGuiKey_RightArrow});
        Press(script, {ImGuiMod_Shift | ImGuiKey_RightArrow, ImGuiMod_Shift | ImGuiKey_DownArrow, ImGuiMod_Shift | ImGuiKey_End});
        Press(script, {ImGuiKey_Home, ImGuiKey_UpArrow});
        Press(script, {ImGuiMod_Alt | ImGuiKey_RightArrow, ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_LeftArrow});
        Press(script, {Ctrl | ImGuiKey_A});
        Idle(script, 3);
        Matches("movement", {.Text = Sample}, script);
    };

    "editing and undo"_test = [] {
        auto script = Focused();
        Type(script, "xy");
        Press(script, {ImGuiKey_Backspace, ImGuiKey_Delete, ImGuiKey_Enter});
        Press(script, {Ctrl | ImGuiKey_Z, Ctrl | ImGuiKey_Z, Ctrl | ImGuiMod_Shift | ImGuiKey_Z});
        Press(script, {ImGuiMod_Shift | ImGuiKey_DownArrow, Ctrl | ImGuiKey_X, Ctrl | ImGuiKey_V, Ctrl | ImGuiKey_V});
        Press(script, {ImGuiMod_Alt | ImGuiKey_UpArrow, ImGuiKey_Tab, ImGuiMod_Shift | ImGuiKey_Tab});
        Idle(script, 3);
        Matches("editing", {.Text = Sample}, script);
    };

    "scrolling"_test = [] {
        auto script = Focused();
        for (int i = 0; i < 5; ++i) script.push_back({Wheel{0, -1}});
        Idle(script, 3);
        script.push_back({Wheel{-2, 0}});
        Idle(script, 3);
        Press(script, {ImGuiKey_PageDown, ImGuiKey_PageDown, ImGuiKey_UpArrow, ImGuiKey_PageUp, Ctrl | ImGuiKey_End});
        Type(script, "end");
        Press(script, {Ctrl | ImGuiKey_Home});
        for (int i = 0; i < 40; ++i) Press(script, {ImGuiKey_DownArrow});
        Press(script, {ImGuiKey_End});
        Idle(script, 3);
        // Drag the vertical scrollbar.
        Drag(script, {{795, 50}, {795, 200}, {795, 300}});
        Idle(script, 3);
        Matches("scrolling", {.Text = Long()}, script);
    };

    "cpp colors and brackets"_test = [] {
        auto script = Focused();
        Press(script, {ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_End});
        Type(script, " (a[b]) {\n");
        Type(script, "}"); // Skips the completed closer.
        Press(script, {ImGuiMod_Alt | ImGuiKey_LeftArrow, ImGuiMod_Super | ImGuiMod_Shift | ImGuiKey_RightArrow, ImGuiMod_Super | ImGuiMod_Shift | ImGuiKey_RightArrow, ImGuiMod_Super | ImGuiMod_Shift | ImGuiKey_LeftArrow});
        Idle(script, 3);
        Click(script, 230, 120); // Near the parameter list.
        Click(script, 230, 120);
        Idle(script, 3);
        Matches("cpp", {.Text = Code, .Config = {.Language = ipte::Cpp()}}, script);
    };

    "every language"_test = [] {
        auto script = Focused();
        Type(script, "x = [1, 2] -- \"s\" --[[ a ]] # t\n");
        Idle(script, 2);
        const auto text = Code + "\n```c\nint x;\n```\n# Heading\n{\"k\": [1, true, null]}\nSELECT * FROM t;\n";
        for (const auto *lang : {ipte::C(), ipte::Cs(), ipte::AngelScript(), ipte::Lua(), ipte::Glsl(), ipte::Hlsl(), ipte::Json(), ipte::Markdown(), ipte::Sql()}) {
            Matches("lang-" + lang->Name, {.Text = text, .Config = {.Language = lang}}, script);
        }
    };

    "python blocks"_test = [] {
        auto script = Focused();
        Press(script, {ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_DownArrow});
        Type(script, "y");
        Idle(script, 3);
        Matches("python", {.Text = PythonCode, .Config = {.Language = ipte::Python()}}, script);
    };

    "find and replace"_test = [] {
        auto script = Focused();
        Press(script, {Ctrl | ImGuiKey_F}); // Opens on the word under the caret.
        Idle(script, 2);
        Type(script, "ur");
        Idle(script, 2);
        Press(script, {ImGuiKey_Enter}); // Back to the editor.
        Press(script, {Ctrl | ImGuiKey_G, Ctrl | ImGuiKey_G, Ctrl | ImGuiMod_Shift | ImGuiKey_F});
        Idle(script, 2);
        Click(script, 708, 27); // Aa
        Click(script, 740, 27); // []
        Click(script, 400, 52); // Replace field.
        Type(script, "X");
        Click(script, 560, 52); // Replace.
        Click(script, 640, 52); // Replace All.
        Idle(script, 2);
        Press(script, {ImGuiKey_Escape});
        Idle(script, 3);
        Matches("find", {.Text = Sample + Sample}, script);
    };

    "occurrences"_test = [] {
        auto script = Focused();
        Press(script, {ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_RightArrow, Ctrl | ImGuiKey_D, Ctrl | ImGuiMod_Shift | ImGuiKey_D});
        Type(script, "y");
        Press(script, {ImGuiKey_Escape});
        Idle(script, 3);
        Matches("occurrences", {.Text = Sample + Sample}, script);
    };

    "settings"_test = [] {
        struct Variant {
            const char *Name;
            ipte::Settings Config;
        };
        const Variant variants[]{
            {"light", {.Colors = ipte::LightPalette()}},
            {"tabs", {.TabSize = 2, .InsertSpacesOnTabs = true}},
            {"spacing", {.LineSpacing = 1.5f}},
            {"no-whitespace", {.ShowSpaces = false, .ShowTabs = false}},
            {"no-line-numbers", {.ShowLineNumbers = false}},
            {"no-highlight", {.ShowCurrentLineHighlight = false, .ShowMatchingBrackets = false}},
            {"no-pairs", {.AutoIndent = false, .CompletePairedGlyphs = false}},
            {"read-only", {.ReadOnly = true}},
            {"overwrite", {.Overwrite = true}},
            {"no-carets", {.CaretsVisible = false}},
            {"margins", {.LeftMargin = 3, .DecorationMargin = 2, .TextMargin = 4}},
            {"no-scrollbar-minimap", {.ShowScrollbarMiniMap = false}},
        };
        auto script = Focused();
        Press(script, {ImGuiKey_End});
        Type(script, " (q) {\n}");
        Press(script, {ImGuiMod_Shift | ImGuiKey_UpArrow});
        Idle(script, 3);
        for (const auto &v : variants) {
            auto config = v.Config;
            config.Language = ipte::Cpp();
            Matches(std::string("settings-") + v.Name, {.Text = Code, .Config = config}, script);
        }
    };

    "word wrap"_test = [] {
        auto script = Focused();
        Press(script, {Ctrl | ImGuiKey_End});
        Type(script, "The quick brown fox jumps over the lazy dog, again and again and again, until the line is far too long to fit on one row of this editor window.");
        Press(script, {ImGuiKey_Home, ImGuiMod_Shift | ImGuiKey_DownArrow, ImGuiMod_Shift | ImGuiKey_RightArrow});
        Idle(script, 2);
        Click(script, 300, 300);
        Click(script, 300, 300);
        Idle(script, 3);
        const auto text = Long() + Code + "\tindented wrapped line with a tab at its start and then enough words to wrap it twice over, surely, by now\n";
        for (const auto annex : {false, true}) {
            Matches(annex ? "wrap-annex14" : "wrap", {.Text = text, .Config = {.WordWrap = true, .Language = ipte::Cpp(), .LineBreak = {.UseUnicodeAnnex14 = annex}}, .DisplaySize = {400, 300}}, script);
        }
    };

    "line folding"_test = [] {
        auto script = Focused();
        Idle(script, 2);
        Click(script, 15, 14); // The fold indicator of main's body.
        Idle(script, 3);
        Press(script, {ImGuiKey_DownArrow, ImGuiKey_DownArrow});
        Type(script, "z");
        Idle(script, 2);
        Click(script, 15, 14);
        Idle(script, 2);
        Click(script, 15, 14);
        Press(script, {ImGuiKey_UpArrow, ImGuiKey_End, ImGuiKey_RightArrow});
        Idle(script, 3);
        Press(script, {Ctrl | ImGuiKey_Z});
        Idle(script, 3);
        Click(script, 15, 14);
        Press(script, {Ctrl | ImGuiKey_Home, ImGuiKey_DownArrow, ImGuiKey_DownArrow});
        Type(script, "\nvoid f() {\n}");
        Idle(script, 3);
        Matches("folding", {.Text = Sample + Sample, .Config = {.LineFolding = true, .Language = ipte::Cpp()}}, script);
    };

    "fold once"_test = [] {
        auto script = Focused();
        Click(script, 15, 14);
        Idle(script, 3);
        Matches("fold-once", {.Text = Sample, .Config = {.LineFolding = true, .Language = ipte::Cpp()}}, script);
    };

    "autocomplete"_test = [] {
        auto script = Focused();
        Press(script, {Ctrl | ImGuiKey_End});
        Type(script, "re");
        Idle(script, 16);
        Press(script, {ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_UpArrow});
        Idle(script, 2);
        Press(script, {ImGuiKey_Enter});
        Idle(script, 3);
        Type(script, " in");
        Idle(script, 16);
        Press(script, {ImGuiKey_Escape});
        Idle(script, 3);
        Type(script, " zz");
        Press(script, {Ctrl | ImGuiKey_Space});
        Idle(script, 16);
        Type(script, "q");
        Idle(script, 3);
        Type(script, " //ar");
        Idle(script, 16);
        Press(script, {ImGuiKey_Enter});
        Type(script, "pr");
        Idle(script, 16);
        Press(script, {ImGuiKey_Tab});
        Idle(script, 3);
        Matches("autocomplete", {.Text = Sample, .Config = {.Language = ipte::Cpp(), .AutoComplete = {.Enabled = true, .Provider = Suggest}}}, script);
    };

    "minimap"_test = [] {
        auto script = Focused();
        Press(script, {ImGuiMod_Shift | ImGuiKey_DownArrow});
        Idle(script, 2);
        Click(script, 790, 400); // Far down the minimap.
        Idle(script, 3);
        Click(script, 790, 300);
        Drag(script, {{790, 60}, {790, 80}, {790, 120}});
        Idle(script, 3);
        Matches("minimap", {.Text = Long() + Code, .Config = {.ShowMiniMap = true, .Language = ipte::Cpp()}}, script);
    };

    "middle mouse pan and scroll"_test = [] {
        for (const auto pan : {true, false}) {
            auto script = Focused();
            script.push_back({MousePos{300, 300}});
            script.push_back({MouseButton{2, true}});
            Idle(script, 2);
            script.push_back({MousePos{300, 200}});
            script.push_back({MousePos{320, 150}});
            Idle(script, 4);
            script.push_back({MouseButton{2, false}});
            Idle(script, 2);
            if (!pan) Click(script, 300, 300);
            Idle(script, 3);
            Matches(pan ? "pan" : "scroll-mode", {.Text = Long(), .Config = {.PanMode = pan}}, script);
        }
    };

    "markers and squiggles"_test = [] {
        auto script = Focused();
        script.push_back({MousePos{100, 28}}); // Over the marked line's text.
        Idle(script, 3);
        script.push_back({MousePos{20, 28}}); // Over its number.
        Idle(script, 3);
        script.push_back({MousePos{60, 67}}); // Over a squiggle.
        Idle(script, 3);
        Click(script, 300, 67);
        Press(script, {ImGuiKey_Home, ImGuiKey_Delete, ImGuiKey_Delete, ImGuiKey_UpArrow, ImGuiKey_End, ImGuiKey_Backspace});
        Press(script, {Ctrl | ImGuiKey_Z, Ctrl | ImGuiKey_Z, Ctrl | ImGuiKey_Z, Ctrl | ImGuiMod_Shift | ImGuiKey_Z});
        Idle(script, 3);
        Setup setup{.Text = Sample, .Config = {.ShowMiniMap = true, .Language = ipte::Cpp()}};
        setup.Markers = {{1, IM_COL32(200, 0, 0, 255), IM_COL32(100, 0, 0, 80), "number tip", "text tip"}, {4, IM_COL32(0, 0, 200, 255), 0, "", ""}};
        setup.Squiggles = {{{4, 0}, {4, 5}, 1, IM_COL32(255, 0, 0, 255), "wavy"}, {{4, 6}, {5, 3}, 2, IM_COL32(0, 255, 0, 100), "", ipte::SquiggleStyle::Background}};
        Matches("markers", setup, script);
    };

    "unicode"_test = [] {
        auto script = Focused();
        Press(script, {Ctrl | ImGuiKey_End});
        Type(script, "日本語 テキスト ünïcödé\tx");
        Press(script, {ImGuiKey_Home, ImGuiKey_RightArrow, ImGuiMod_Shift | ImGuiKey_RightArrow, ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_RightArrow});
        Press(script, {Ctrl | ImGuiKey_C, ImGuiKey_End, Ctrl | ImGuiKey_V, ImGuiMod_Alt | ImGuiKey_Backspace});
        Idle(script, 3);
        Matches("unicode", {.Text = "int 変数 = 1; // コメント\n\t\"文字列\" 'c'\nΩ ≈ α + β\n", .Config = {.WordWrap = true, .Language = ipte::Cpp()}, .DisplaySize = {300, 200}}, script);
    };

    "wrap, folding and minimap together"_test = [] {
        auto script = Focused();
        Click(script, 15, 14);
        Idle(script, 2);
        Press(script, {ImGuiKey_DownArrow, ImGuiKey_DownArrow, ImGuiKey_End});
        Type(script, " and a long tail of words that will certainly wrap around the narrow window more than once");
        Press(script, {ImGuiKey_UpArrow, ImGuiMod_Shift | ImGuiKey_UpArrow});
        for (int i = 0; i < 3; ++i) script.push_back({Wheel{0, -1}});
        Idle(script, 3);
        Matches("combined", {.Text = Sample + Code + Sample, .Config = {.WordWrap = true, .ShowMiniMap = true, .LineFolding = true, .Language = ipte::Cpp()}, .DisplaySize = {360, 240}}, script);
    };

    "font size, dpi scale, style alpha and keyboard navigation"_test = [] {
        auto script = Focused();
        Type(script, "abc");
        Press(script, {Ctrl | ImGuiKey_F});
        Idle(script, 3);
        Press(script, {ImGuiKey_Escape, ImGuiMod_Shift | ImGuiKey_DownArrow});
        for (int i = 0; i < 3; ++i) script.push_back({Wheel{0, -1}});
        Idle(script, 3);
        Matches("scaled", {.Text = Long() + Code, .Config = {.ShowMiniMap = true, .LineFolding = true, .Language = ipte::Cpp()}, .FontSize = 20, .FontScaleDpi = 1.5f, .StyleAlpha = 0.8f, .NavKeyboard = true}, script);
    };

    "read-only find window"_test = [] {
        auto script = Focused();
        Press(script, {Ctrl | ImGuiKey_F});
        Type(script, "line");
        Press(script, {ImGuiKey_Enter, Ctrl | ImGuiKey_G});
        Idle(script, 3);
        Matches("read-only-find", {.Text = Sample, .Config = {.ReadOnly = true}}, script);
    };

    "mouse selection"_test = [] {
        auto script = Focused();
        Click(script, 300, 100); // Second click of a double click.
        Idle(script, 2);
        Drag(script, {{320, 100}, {400, 130}, {420, 150}});
        Idle(script, 2);
        Click(script, 10, 60); // Line numbers.
        Click(script, 300, 20, 0, ImGuiMod_Alt); // Adds a cursor.
        Type(script, "Q");
        Idle(script, 3);
        Matches("mouse", {.Text = Sample}, script);
    };
}
