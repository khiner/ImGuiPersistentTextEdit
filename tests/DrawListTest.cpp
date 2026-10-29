// Compares the draw data of upstream's editor and ours over scripted interactions.
//   cmake --build build --target DrawListTest && ./build/tests/DrawListTest

#include "Harness.h"

#include <boost/ut.hpp>

using namespace boost::ut;

namespace {
void Matches(const std::string &name, const harness::Setup &setup, const harness::Script &script) {
    std::string report;
    const auto frame = harness::Run(name, setup, script, report);
    expect(frame == -1) << name << ": " << report << " (see " IPTE_BINARY_DIR "/mismatch/" << name << ")";
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
    for (const auto &w : words) {
        bool match = context.SearchTerm.size() <= w.size();
        for (size_t i = 0; match && i < context.SearchTerm.size(); ++i) match = std::tolower(context.SearchTerm[i]) == w[i];
        if (match) out.push_back(w);
    }
    return out;
}

harness::Script Focused() {
    harness::Script s;
    harness::Idle(s, 2);
    harness::Click(s, 300, 100);
    harness::Idle(s, 2);
    return s;
}
} // namespace

int main() {
    "empty document"_test = [] {
        harness::Script script;
        harness::Idle(script, 3);
        Matches("empty", {.Text = ""}, script);
    };

    "harness sees a difference"_test = [] {
        harness::Script script;
        harness::Idle(script, 2);
        std::string report;
        const auto frame = harness::Run("negative", {.Text = "\tx", .Upstream = [](TextEditor &e) { e.SetTabSize(2); }}, script, report);
        expect(frame == 0) << report;
    };

    "typing reaches the document"_test = [] {
        auto script = Focused();
        harness::Type(script, "z");
        std::string report;
        const auto frame = harness::Run("negative-typing", {.Text = Sample, .Upstream = [](TextEditor &e) { e.SetReadOnlyEnabled(true); }}, script, report);
        expect(frame == int(script.size()) - 1) << report;
    };

    "plain text"_test = [] {
        harness::Script script;
        harness::Idle(script, 3);
        Matches("plain", {.Text = "hello world\n\tsecond line\n"}, script);
    };

    "click focuses and places the caret"_test = [] {
        auto script = Focused();
        harness::Idle(script, 60); // Longer than one caret blink.
        Matches("click", {.Text = Sample}, script);
    };

    "typing"_test = [] {
        auto script = Focused();
        harness::Type(script, "abc def\n\tghi (x) {\n}");
        harness::Idle(script, 3);
        Matches("typing", {.Text = Sample}, script);
    };

    "movement and selection"_test = [] {
        auto script = Focused();
        for (int i = 0; i < 3; ++i) harness::Press(script, ImGuiKey_RightArrow);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_DownArrow, ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_End, ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_Home);
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Alt);
        harness::Press(script, ImGuiKey_LeftArrow, ImGuiMod_Alt | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_A, Ctrl);
        harness::Idle(script, 3);
        Matches("movement", {.Text = Sample}, script);
    };

    "editing and undo"_test = [] {
        auto script = Focused();
        harness::Type(script, "xy");
        harness::Press(script, ImGuiKey_Backspace);
        harness::Press(script, ImGuiKey_Delete);
        harness::Press(script, ImGuiKey_Enter);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Press(script, ImGuiKey_Z, Ctrl | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_DownArrow, ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_X, Ctrl);
        harness::Press(script, ImGuiKey_V, Ctrl);
        harness::Press(script, ImGuiKey_V, Ctrl);
        harness::Press(script, ImGuiKey_UpArrow, ImGuiMod_Alt);
        harness::Press(script, ImGuiKey_Tab);
        harness::Press(script, ImGuiKey_Tab, ImGuiMod_Shift);
        harness::Idle(script, 3);
        Matches("editing", {.Text = Sample}, script);
    };

    "scrolling"_test = [] {
        auto script = Focused();
        for (int i = 0; i < 5; ++i) script.push_back({harness::Wheel{0, -1}});
        harness::Idle(script, 3);
        script.push_back({harness::Wheel{-2, 0}});
        harness::Idle(script, 3);
        harness::Press(script, ImGuiKey_PageDown);
        harness::Press(script, ImGuiKey_PageDown);
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Press(script, ImGuiKey_PageUp);
        harness::Press(script, ImGuiKey_End, Ctrl);
        harness::Type(script, "end");
        harness::Press(script, ImGuiKey_Home, Ctrl);
        for (int i = 0; i < 40; ++i) harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_End);
        harness::Idle(script, 3);
        // Drag the vertical scrollbar.
        script.push_back({harness::MousePos{795, 50}});
        script.push_back({harness::MouseButton{0, true}});
        script.push_back({harness::MousePos{795, 200}});
        script.push_back({harness::MousePos{795, 300}});
        script.push_back({harness::MouseButton{0, false}});
        harness::Idle(script, 3);
        Matches("scrolling", {.Text = Long()}, script);
    };

    "cpp colors and brackets"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_End);
        harness::Type(script, " (a[b]) {");
        harness::Press(script, ImGuiKey_Enter);
        harness::Type(script, "}"); // Skips the completed closer.
        harness::Press(script, ImGuiKey_LeftArrow, ImGuiMod_Alt);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Super | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Super | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_LeftArrow, ImGuiMod_Super | ImGuiMod_Shift);
        harness::Idle(script, 3);
        harness::Click(script, 230, 120); // Near the parameter list.
        harness::Click(script, 230, 120);
        harness::Idle(script, 3);
        Matches("cpp", {.Text = Code, .Config = {.Language = ipte::Cpp()}}, script);
    };

    "every language"_test = [] {
        for (const auto *lang : {ipte::C(), ipte::Cs(), ipte::AngelScript(), ipte::Lua(), ipte::Glsl(), ipte::Hlsl(), ipte::Json(), ipte::Markdown(), ipte::Sql()}) {
            auto script = Focused();
            harness::Type(script, "x = [1, 2] -- \"s\" --[[ a ]] # t\n");
            harness::Idle(script, 2);
            Matches("lang-" + lang->Name, {.Text = Code + "\n```c\nint x;\n```\n# Heading\n{\"k\": [1, true, null]}\nSELECT * FROM t;\n", .Config = {.Language = lang}}, script);
        }
    };

    "python blocks"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Type(script, "y");
        harness::Idle(script, 3);
        Matches("python", {.Text = PythonCode, .Config = {.Language = ipte::Python()}}, script);
    };

    "find and replace"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_F, Ctrl); // Opens on the word under the caret.
        harness::Idle(script, 2);
        harness::Type(script, "ur");
        harness::Idle(script, 2);
        harness::Press(script, ImGuiKey_Enter); // Back to the editor.
        harness::Press(script, ImGuiKey_G, Ctrl);
        harness::Press(script, ImGuiKey_G, Ctrl);
        harness::Press(script, ImGuiKey_F, Ctrl | ImGuiMod_Shift);
        harness::Idle(script, 2);
        harness::Click(script, 708, 27); // Aa
        harness::Click(script, 740, 27); // []
        harness::Click(script, 400, 52); // Replace field.
        harness::Type(script, "X");
        harness::Click(script, 560, 52); // Replace.
        harness::Click(script, 640, 52); // Replace All.
        harness::Idle(script, 2);
        harness::Press(script, ImGuiKey_Escape);
        harness::Idle(script, 3);
        Matches("find", {.Text = Sample + Sample}, script);
    };

    "occurrences"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Alt | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_D, Ctrl);
        harness::Press(script, ImGuiKey_D, Ctrl | ImGuiMod_Shift);
        harness::Type(script, "y");
        harness::Press(script, ImGuiKey_Escape);
        harness::Idle(script, 3);
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
            {"no-pairs", {.CompletePairedGlyphs = false, .AutoIndent = false}},
            {"read-only", {.ReadOnly = true}},
            {"overwrite", {.Overwrite = true}},
            {"no-carets", {.CaretsVisible = false}},
            {"margins", {.LeftMargin = 3, .DecorationMargin = 2, .TextMargin = 4}},
            {"no-scrollbar-minimap", {.ShowScrollbarMiniMap = false}},
        };
        for (const auto &v : variants) {
            auto script = Focused();
            harness::Press(script, ImGuiKey_End);
            harness::Type(script, " (q) {\n}");
            harness::Press(script, ImGuiKey_UpArrow, ImGuiMod_Shift);
            harness::Idle(script, 3);
            auto config = v.Config;
            config.Language = ipte::Cpp();
            Matches(std::string("settings-") + v.Name, {.Text = Code, .Config = config}, script);
        }
    };

    "word wrap"_test = [] {
        for (const auto annex : {false, true}) {
            auto script = Focused();
            harness::Press(script, ImGuiKey_End, Ctrl);
            harness::Type(script, "The quick brown fox jumps over the lazy dog, again and again and again, until the line is far too long to fit on one row of this editor window.");
            harness::Press(script, ImGuiKey_Home);
            harness::Press(script, ImGuiKey_DownArrow, ImGuiMod_Shift);
            harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Shift);
            harness::Idle(script, 2);
            harness::Click(script, 300, 300);
            harness::Click(script, 300, 300);
            harness::Idle(script, 3);
            ipte::Settings config{.WordWrap = true, .Language = ipte::Cpp()};
            config.LineBreak.UseUnicodeAnnex14 = annex;
            Matches(annex ? "wrap-annex14" : "wrap", {.Text = Long() + Code + "\tindented wrapped line with a tab at its start and then enough words to wrap it twice over, surely, by now\n", .Config = config, .DisplaySize = {400, 300}}, script);
        }
    };

    "line folding"_test = [] {
        auto script = Focused();
        harness::Idle(script, 2);
        harness::Click(script, 15, 14); // The fold indicator of main's body.
        harness::Idle(script, 3);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Type(script, "z");
        harness::Idle(script, 2);
        harness::Click(script, 15, 14);
        harness::Idle(script, 2);
        harness::Click(script, 15, 14);
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Press(script, ImGuiKey_End);
        harness::Press(script, ImGuiKey_RightArrow);
        harness::Idle(script, 3);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Idle(script, 3);
        harness::Click(script, 15, 14);
        harness::Press(script, ImGuiKey_Home, Ctrl);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_Enter);
        harness::Type(script, "void f() {\n}");
        harness::Idle(script, 3);
        Matches("folding", {.Text = Sample + Sample, .Config = {.LineFolding = true, .Language = ipte::Cpp()}}, script);
    };

    "fold once"_test = [] {
        auto script = Focused();
        harness::Click(script, 15, 14);
        harness::Idle(script, 3);
        Matches("fold-once", {.Text = Sample, .Config = {.LineFolding = true, .Language = ipte::Cpp()}}, script);
    };

    "autocomplete"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_End, Ctrl);
        harness::Type(script, "re");
        harness::Idle(script, 16);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Idle(script, 2);
        harness::Press(script, ImGuiKey_Enter);
        harness::Idle(script, 3);
        harness::Type(script, " in");
        harness::Idle(script, 16);
        harness::Press(script, ImGuiKey_Escape);
        harness::Idle(script, 3);
        harness::Type(script, " zz");
        harness::Press(script, ImGuiKey_Space, Ctrl);
        harness::Idle(script, 16);
        harness::Type(script, "q");
        harness::Idle(script, 3);
        harness::Type(script, " //ar");
        harness::Idle(script, 16);
        harness::Press(script, ImGuiKey_Enter);
        harness::Type(script, "pr");
        harness::Idle(script, 16);
        harness::Press(script, ImGuiKey_Tab);
        harness::Idle(script, 3);
        ipte::Settings config{.Language = ipte::Cpp()};
        config.AutoComplete.Enabled = true;
        config.AutoComplete.Provider = Suggest;
        Matches("autocomplete", {.Text = Sample, .Config = config}, script);
    };

    "minimap"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_DownArrow, ImGuiMod_Shift);
        harness::Idle(script, 2);
        harness::Click(script, 790, 400); // Far down the minimap.
        harness::Idle(script, 3);
        harness::Click(script, 790, 300);
        script.push_back({harness::MousePos{790, 60}});
        script.push_back({harness::MouseButton{0, true}});
        script.push_back({harness::MousePos{790, 80}});
        script.push_back({harness::MousePos{790, 120}});
        script.push_back({harness::MouseButton{0, false}});
        harness::Idle(script, 3);
        Matches("minimap", {.Text = Long() + Code, .Config = {.ShowMiniMap = true, .Language = ipte::Cpp()}}, script);
    };

    "middle mouse pan and scroll"_test = [] {
        for (const auto pan : {true, false}) {
            auto script = Focused();
            script.push_back({harness::MousePos{300, 300}});
            script.push_back({harness::MouseButton{2, true}});
            harness::Idle(script, 2);
            script.push_back({harness::MousePos{300, 200}});
            script.push_back({harness::MousePos{320, 150}});
            harness::Idle(script, 4);
            script.push_back({harness::MouseButton{2, false}});
            harness::Idle(script, 2);
            if (!pan) harness::Click(script, 300, 300);
            harness::Idle(script, 3);
            Matches(pan ? "pan" : "scroll-mode", {.Text = Long(), .Config = {.PanMode = pan}}, script);
        }
    };

    "markers and squiggles"_test = [] {
        auto script = Focused();
        script.push_back({harness::MousePos{100, 28}}); // Over the marked line's text.
        harness::Idle(script, 3);
        script.push_back({harness::MousePos{20, 28}}); // Over its number.
        harness::Idle(script, 3);
        script.push_back({harness::MousePos{60, 67}}); // Over a squiggle.
        harness::Idle(script, 3);
        harness::Click(script, 300, 67);
        harness::Press(script, ImGuiKey_Home);
        harness::Press(script, ImGuiKey_Delete);
        harness::Press(script, ImGuiKey_Delete);
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Press(script, ImGuiKey_End);
        harness::Press(script, ImGuiKey_Backspace);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Press(script, ImGuiKey_Z, Ctrl);
        harness::Press(script, ImGuiKey_Z, Ctrl | ImGuiMod_Shift);
        harness::Idle(script, 3);
        harness::Setup setup{.Text = Sample, .Config = {.ShowMiniMap = true, .Language = ipte::Cpp()}};
        setup.Markers = {{1, IM_COL32(200, 0, 0, 255), IM_COL32(100, 0, 0, 80), "number tip", "text tip"}, {4, IM_COL32(0, 0, 200, 255), 0, "", ""}};
        setup.Squiggles = {{{4, 0}, {4, 5}, 1, IM_COL32(255, 0, 0, 255), "wavy"}, {{4, 6}, {5, 3}, 2, IM_COL32(0, 255, 0, 100), "", ipte::SquiggleStyle::Background}};
        Matches("markers", setup, script);
    };

    "unicode"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_End, Ctrl);
        harness::Type(script, "日本語 テキスト ünïcödé\tx");
        harness::Press(script, ImGuiKey_Home);
        harness::Press(script, ImGuiKey_RightArrow);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_RightArrow, ImGuiMod_Alt | ImGuiMod_Shift);
        harness::Press(script, ImGuiKey_C, Ctrl);
        harness::Press(script, ImGuiKey_End);
        harness::Press(script, ImGuiKey_V, Ctrl);
        harness::Press(script, ImGuiKey_Backspace, ImGuiMod_Alt);
        harness::Idle(script, 3);
        Matches("unicode", {.Text = "int 変数 = 1; // コメント\n\t\"文字列\" 'c'\nΩ ≈ α + β\n", .Config = {.WordWrap = true, .Language = ipte::Cpp()}, .DisplaySize = {300, 200}}, script);
    };

    "wrap, folding and minimap together"_test = [] {
        auto script = Focused();
        harness::Click(script, 15, 14);
        harness::Idle(script, 2);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_DownArrow);
        harness::Press(script, ImGuiKey_End);
        harness::Type(script, " and a long tail of words that will certainly wrap around the narrow window more than once");
        harness::Press(script, ImGuiKey_UpArrow);
        harness::Press(script, ImGuiKey_UpArrow, ImGuiMod_Shift);
        for (int i = 0; i < 3; ++i) script.push_back({harness::Wheel{0, -1}});
        harness::Idle(script, 3);
        Matches("combined", {.Text = Sample + Code + Sample, .Config = {.WordWrap = true, .ShowMiniMap = true, .LineFolding = true, .Language = ipte::Cpp()}, .DisplaySize = {360, 240}}, script);
    };

    "font size, dpi scale, style alpha and keyboard navigation"_test = [] {
        auto script = Focused();
        harness::Type(script, "abc");
        harness::Press(script, ImGuiKey_F, Ctrl);
        harness::Idle(script, 3);
        harness::Press(script, ImGuiKey_Escape);
        harness::Press(script, ImGuiKey_DownArrow, ImGuiMod_Shift);
        for (int i = 0; i < 3; ++i) script.push_back({harness::Wheel{0, -1}});
        harness::Idle(script, 3);
        harness::Setup setup{.Text = Long() + Code, .Config = {.ShowMiniMap = true, .LineFolding = true, .Language = ipte::Cpp()}};
        setup.FontSize = 20;
        setup.FontScaleDpi = 1.5f;
        setup.StyleAlpha = 0.8f;
        setup.NavKeyboard = true;
        Matches("scaled", setup, script);
    };

    "read-only find window"_test = [] {
        auto script = Focused();
        harness::Press(script, ImGuiKey_F, Ctrl);
        harness::Type(script, "line");
        harness::Press(script, ImGuiKey_Enter);
        harness::Press(script, ImGuiKey_G, Ctrl);
        harness::Idle(script, 3);
        Matches("read-only-find", {.Text = Sample, .Config = {.ReadOnly = true}}, script);
    };

    "mouse selection"_test = [] {
        auto script = Focused();
        harness::Click(script, 300, 100); // Second click of a double click.
        harness::Idle(script, 2);
        script.push_back({harness::MousePos{320, 100}});
        script.push_back({harness::MouseButton{0, true}});
        script.push_back({harness::MousePos{400, 130}});
        script.push_back({harness::MousePos{420, 150}});
        script.push_back({harness::MouseButton{0, false}});
        harness::Idle(script, 2);
        harness::Click(script, 10, 60); // Line numbers.
        harness::Click(script, 300, 20, 0, ImGuiMod_Alt); // Adds a cursor.
        harness::Type(script, "Q");
        harness::Idle(script, 3);
        Matches("mouse", {.Text = Sample}, script);
    };
}
