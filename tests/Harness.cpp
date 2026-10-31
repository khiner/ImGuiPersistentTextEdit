#include "Harness.h"

#include "HarnessClock.h"
#include "imgui_internal.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <new>
#include <ranges>

namespace harness {
namespace {
struct Cmd {
    ImVec4 ClipRect;
    ImTextureID Tex;
    unsigned VtxOffset, IdxOffset, ElemCount;
};
struct List {
    std::vector<ImDrawVert> Vertices;
    std::vector<ImDrawIdx> Indices;
    std::vector<Cmd> Cmds;
};
using Capture = std::vector<List>;

Capture Captured(const ImDrawData *data) {
    Capture out;
    for (const auto *list : data->CmdLists) {
        List l;
        l.Vertices.assign(list->VtxBuffer.begin(), list->VtxBuffer.end());
        l.Indices.assign(list->IdxBuffer.begin(), list->IdxBuffer.end());
        for (const auto &c : list->CmdBuffer) l.Cmds.push_back({c.ClipRect, c.GetTexID(), c.VtxOffset, c.IdxOffset, c.ElemCount});
        out.push_back(std::move(l));
    }
    return out;
}

std::string Differ(const Capture &a, const Capture &b) {
    if (a.size() != b.size()) return std::format("{} draw lists vs {}", a.size(), b.size());
    for (size_t l = 0; l < a.size(); ++l) {
        const auto &x = a[l], &y = b[l];
        if (x.Cmds.size() != y.Cmds.size()) return std::format("list {}: {} commands vs {}", l, x.Cmds.size(), y.Cmds.size());
        for (size_t c = 0; c < x.Cmds.size(); ++c) {
            const auto &p = x.Cmds[c], &q = y.Cmds[c];
            if (std::memcmp(&p.ClipRect, &q.ClipRect, sizeof p.ClipRect) != 0)
                return std::format("list {} command {}: clip ({}, {}, {}, {}) vs ({}, {}, {}, {})", l, c, p.ClipRect.x, p.ClipRect.y, p.ClipRect.z, p.ClipRect.w, q.ClipRect.x, q.ClipRect.y, q.ClipRect.z, q.ClipRect.w);
            if (p.Tex != q.Tex) return std::format("list {} command {}: texture", l, c);
            if (p.VtxOffset != q.VtxOffset || p.IdxOffset != q.IdxOffset || p.ElemCount != q.ElemCount)
                return std::format("list {} command {}: {} elements at {} vs {} at {}", l, c, p.ElemCount, p.IdxOffset, q.ElemCount, q.IdxOffset);
        }
        if (x.Vertices.size() != y.Vertices.size()) return std::format("list {}: {} vertices vs {}", l, x.Vertices.size(), y.Vertices.size());
        for (size_t v = 0; v < x.Vertices.size(); ++v) {
            const auto &p = x.Vertices[v], &q = y.Vertices[v];
            if (std::memcmp(&p, &q, sizeof p) != 0)
                return std::format("list {} vertex {}: pos ({}, {}) uv ({}, {}) col {:08x} vs pos ({}, {}) uv ({}, {}) col {:08x}", l, v, p.pos.x, p.pos.y, p.uv.x, p.uv.y, p.col, q.pos.x, q.pos.y, q.uv.x, q.uv.y, q.col);
        }
        if (x.Indices != y.Indices) return std::format("list {}: indices", l);
    }
    return {};
}

// Rasterizes a capture on the CPU for inspection. Approximate.
struct Image {
    int W, H;
    std::vector<unsigned char> Rgb;
};

Image Rasterized(const Capture &capture, const ImTextureData *tex, int w, int h) {
    const auto *pixels = static_cast<const unsigned char *>(tex->Pixels);
    const int texW = tex->Width, texH = tex->Height, bpp = tex->BytesPerPixel;
    Image img{w, h, std::vector<unsigned char>(size_t(w) * h * 3, 20)};
    for (const auto &list : capture) {
        for (const auto &cmd : list.Cmds) {
            const int cx0 = int(cmd.ClipRect.x), cy0 = int(cmd.ClipRect.y), cx1 = int(cmd.ClipRect.z), cy1 = int(cmd.ClipRect.w);
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                const auto &a = list.Vertices[cmd.VtxOffset + list.Indices[cmd.IdxOffset + i]];
                const auto &b = list.Vertices[cmd.VtxOffset + list.Indices[cmd.IdxOffset + i + 1]];
                const auto &c = list.Vertices[cmd.VtxOffset + list.Indices[cmd.IdxOffset + i + 2]];
                const int x0 = std::max(cx0, int(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x})))), x1 = std::min(cx1 - 1, int(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
                const int y0 = std::max(cy0, int(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y})))), y1 = std::min(cy1 - 1, int(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
                const float area = (b.pos.x - a.pos.x) * (c.pos.y - a.pos.y) - (b.pos.y - a.pos.y) * (c.pos.x - a.pos.x);
                if (area == 0) continue;
                for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y) {
                    for (int x = std::max(0, x0); x <= std::min(w - 1, x1); ++x) {
                        const float px = x + 0.5f, py = y + 0.5f;
                        float wa = ((b.pos.x - px) * (c.pos.y - py) - (b.pos.y - py) * (c.pos.x - px)) / area;
                        float wb = ((c.pos.x - px) * (a.pos.y - py) - (c.pos.y - py) * (a.pos.x - px)) / area;
                        float wc = 1 - wa - wb;
                        if (wa < 0 || wb < 0 || wc < 0) continue;
                        // Use the single uv of a solid primitive directly. Interpolation drifts.
                        const bool solid = a.uv.x == b.uv.x && a.uv.x == c.uv.x && a.uv.y == b.uv.y && a.uv.y == c.uv.y;
                        const float u = solid ? a.uv.x : wa * a.uv.x + wb * b.uv.x + wc * c.uv.x, v = solid ? a.uv.y : wa * a.uv.y + wb * b.uv.y + wc * c.uv.y;
                        const int tx = std::clamp(int(u * texW), 0, texW - 1), ty = std::clamp(int(v * texH), 0, texH - 1);
                        const float alpha = pixels[(ty * texW + tx) * bpp + bpp - 1] / 255.f;
                        auto channel = [&](int shift) { return (wa * ((a.col >> shift) & 255) + wb * ((b.col >> shift) & 255) + wc * ((c.col >> shift) & 255)) / 255.f; };
                        const float al = channel(IM_COL32_A_SHIFT) * alpha;
                        auto *dst = &img.Rgb[(size_t(y) * w + x) * 3];
                        const float rgb[]{channel(IM_COL32_R_SHIFT), channel(IM_COL32_G_SHIFT), channel(IM_COL32_B_SHIFT)};
                        for (int k = 0; k < 3; ++k) dst[k] = (unsigned char)std::clamp(dst[k] * (1 - al) + rgb[k] * 255 * al, 0.f, 255.f);
                    }
                }
            }
        }
    }
    return img;
}

unsigned Crc(const unsigned char *d, size_t n) {
    unsigned crc = ~0u;
    for (size_t i = 0; i < n; ++i) {
        crc ^= d[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// Appends `v` as four big-endian bytes.
void Put32(std::vector<unsigned char> &out, unsigned v) { out.insert(out.end(), {(unsigned char)(v >> 24), (unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v}); }

void Chunk(std::vector<unsigned char> &png, const char *type, const std::vector<unsigned char> &data) {
    Put32(png, unsigned(data.size()));
    const auto start = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    Put32(png, Crc(png.data() + start, png.size() - start));
}

void WritePng(const std::filesystem::path &path, const Image &img) {
    std::vector<unsigned char> png{137, 80, 78, 71, 13, 10, 26, 10}, ihdr;
    Put32(ihdr, img.W);
    Put32(ihdr, img.H);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    Chunk(png, "IHDR", ihdr);
    std::vector<unsigned char> raw;
    for (int y = 0; y < img.H; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), img.Rgb.begin() + size_t(y) * img.W * 3, img.Rgb.begin() + size_t(y + 1) * img.W * 3);
    }
    std::vector<unsigned char> z{0x78, 0x01};
    for (size_t at = 0; at < raw.size();) {
        const size_t n = std::min<size_t>(65535, raw.size() - at);
        const bool last = at + n == raw.size();
        z.push_back(last);
        z.push_back(n & 255);
        z.push_back(n >> 8);
        z.push_back(~n & 255);
        z.push_back((~n >> 8) & 255);
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    }
    unsigned a = 1, b = 0;
    for (const auto x : raw) {
        a = (a + x) % 65521;
        b = (b + a) % 65521;
    }
    Put32(z, (b << 16) | a);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", {});
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
}

// Writes `capture` as a PNG, rasterized with the font atlas of `ctx`.
void WriteFrame(const std::filesystem::path &path, ImGuiContext *ctx, const Capture &capture, ImVec2 size) {
    std::filesystem::create_directories(path.parent_path());
    ImGui::SetCurrentContext(ctx);
    WritePng(path, Rasterized(capture, ImGui::GetIO().Fonts->TexData, int(size.x), int(size.y)));
}

struct Side {
    ImGuiContext *Ctx;
    std::string Clipboard;

    explicit Side(const Setup &setup) : Ctx(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(Ctx);
        auto &io = ImGui::GetIO();
        io.DisplaySize = setup.DisplaySize;
        if (setup.NavKeyboard) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::GetStyle().FontScaleDpi = setup.FontScaleDpi;
        ImGui::GetStyle().Alpha = setup.StyleAlpha;
        io.DeltaTime = 1.f / 60;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // Required to build fonts without a renderer backend.
        io.Fonts->AddFontDefault();
        auto &platform = ImGui::GetPlatformIO();
        platform.Platform_ClipboardUserData = this;
        platform.Platform_GetClipboardTextFn = [](ImGuiContext *) { return static_cast<Side *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->Clipboard.c_str(); };
        platform.Platform_SetClipboardTextFn = [](ImGuiContext *, const char *text) { static_cast<Side *>(ImGui::GetPlatformIO().Platform_ClipboardUserData)->Clipboard = text; };
    }
    ~Side() { ImGui::DestroyContext(Ctx); }

    void Feed(const Frame &frame) {
        auto &io = ImGui::GetIO();
        for (const auto &e : frame) {
            std::visit([&](const auto &ev) {
                using T = std::decay_t<decltype(ev)>;
                if constexpr (std::is_same_v<T, Key>) io.AddKeyEvent(ev.Code, ev.Down);
                else if constexpr (std::is_same_v<T, Char>) io.AddInputCharacter(ev.Codepoint);
                else if constexpr (std::is_same_v<T, MousePos>) io.AddMousePosEvent(ev.X, ev.Y);
                else if constexpr (std::is_same_v<T, MouseButton>) io.AddMouseButtonEvent(ev.Button, ev.Down);
                else if constexpr (std::is_same_v<T, Wheel>) io.AddMouseWheelEvent(ev.X, ev.Y);
                else if constexpr (std::is_same_v<T, Focus>) io.AddFocusEvent(ev.Focused);
            },
                       e);
        }
    }

    template<typename F> Capture Run(const Frame &frame, const Setup &setup, F &&editor) {
        ImGui::SetCurrentContext(Ctx);
        Feed(frame);
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::Begin("Host", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (setup.FontSize) ImGui::PushFont(nullptr, setup.FontSize);
        editor();
        if (setup.FontSize) ImGui::PopFont();
        ImGui::End();
        ImGui::Render();
        for (auto *tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_WantCreate) tex->SetTexID(ImTextureID(1));
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) tex->SetStatus(ImTextureStatus_OK);
            else if (tex->Status == ImTextureStatus_WantDestroy) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        return Captured(ImGui::GetDrawData());
    }
};

void Mods(Script &s, int mods, bool down) {
    for (const auto m : {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super})
        if (mods & m) s.back().push_back(Key{m, down});
}

// Appends a frame pressing `mods` and `down`, then a frame with `up` releasing them.
void Stroke(Script &s, int mods, Event down, Event up) {
    s.emplace_back();
    Mods(s, mods, true);
    s.back().push_back(down);
    s.push_back({up});
    Mods(s, mods, false);
}
} // namespace

void Press(Script &s, std::initializer_list<ImGuiKeyChord> chords) {
    for (const auto chord : chords) {
        const auto key = ImGuiKey(chord & ~ImGuiMod_Mask_);
        Stroke(s, chord & ImGuiMod_Mask_, Key{key, true}, Key{key, false});
    }
}

void Type(Script &s, std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        unsigned cp;
        const auto n = ImTextCharFromUtf8(&cp, utf8.data() + i, utf8.data() + utf8.size());
        i += n ? n : 1;
        if (cp == '\n') Press(s, {ImGuiKey_Enter});
        else s.push_back({Char{cp}});
    }
}

void Click(Script &s, float x, float y, int button, int mods) {
    s.push_back({MousePos{x, y}});
    Stroke(s, mods, MouseButton{button, true}, MouseButton{button, false});
}

void Drag(Script &s, std::initializer_list<ImVec2> path) {
    s.push_back({MousePos{path.begin()->x, path.begin()->y}});
    s.push_back({MouseButton{0, true}});
    for (const auto &p : path | std::views::drop(1)) s.push_back({MousePos{p.x, p.y}});
    s.push_back({MouseButton{0, false}});
}

void Idle(Script &s, int frames) {
    for (int i = 0; i < frames; ++i) s.emplace_back();
}

int Run(const std::string &name, const Setup &setup, const Script &script, std::string &report) {
    Side theirs(setup), ours(setup);

    // Zeroed storage, because upstream's first frame reads an uninitialized palette. See UpstreamBugs.md.
    ImGui::SetCurrentContext(theirs.Ctx);
    alignas(TextEditor) static unsigned char storage[sizeof(TextEditor)];
    std::memset(storage, 0, sizeof storage);
    auto *upstream = new (storage) TextEditor();
    {
        const auto &c = setup.Config;
        upstream->SetTabSize(c.TabSize);
        upstream->SetInsertSpacesOnTabs(c.InsertSpacesOnTabs);
        upstream->SetLineSpacing(c.LineSpacing);
        upstream->SetWordWrapEnabled(c.WordWrap);
        upstream->SetReadOnlyEnabled(c.ReadOnly);
        upstream->SetCaretsVisible(c.CaretsVisible);
        upstream->SetAutoIndentEnabled(c.AutoIndent);
        upstream->SetShowSpacesEnabled(c.ShowSpaces);
        upstream->SetShowTabsEnabled(c.ShowTabs);
        upstream->SetShowLineNumbersEnabled(c.ShowLineNumbers);
        upstream->SetShowMiniMapEnabled(c.ShowMiniMap);
        upstream->SetMiniMapColumns(c.MiniMapColumns);
        upstream->SetShowScrollbarMiniMapEnabled(c.ShowScrollbarMiniMap);
        upstream->SetShowPanScrollIndicatorEnabled(c.ShowPanScrollIndicator);
        upstream->SetShowCurrentLineHighlightEnabled(c.ShowCurrentLineHighlight);
        upstream->SetShowMatchingBrackets(c.ShowMatchingBrackets);
        upstream->SetCompletePairedGlyphs(c.CompletePairedGlyphs);
        upstream->SetLineFoldingEnabled(c.LineFolding);
        upstream->SetOverwriteEnabled(c.Overwrite);
        if (c.PanMode) upstream->SetMiddleMousePanMode();
        else upstream->SetMiddleMouseScrollMode();
        upstream->SetFindReplaceEnabled(c.FindReplaceEnabled);
        upstream->SetLineNumberLeftMargin(c.LeftMargin);
        upstream->SetDecorationLeftMargin(c.DecorationMargin);
        upstream->SetTextLeftMargin(c.TextMargin);
        upstream->SetPalette({c.Colors});
        TextEditor::LineBreakConfig lineBreak{.useUnicodeAnnex14 = c.LineBreak.UseUnicodeAnnex14, .breakAfter = c.LineBreak.BreakAfter, .breakBefore = c.LineBreak.BreakBefore};
        upstream->SetLineBreakConfig(lineBreak);
        if (c.AutoComplete.Enabled) {
            static TextEditor::AutoCompleteConfig autoComplete;
            static std::vector<std::string> (*provider)(const ipte::AutoCompleteContext &) = nullptr;
            provider = c.AutoComplete.Provider;
            autoComplete.triggerOnTyping = c.AutoComplete.TriggerOnTyping;
            autoComplete.triggerOnShortcut = c.AutoComplete.TriggerOnShortcut;
            autoComplete.triggerInComments = c.AutoComplete.TriggerInComments;
            autoComplete.triggerInStrings = c.AutoComplete.TriggerInStrings;
            autoComplete.triggerShortcut = c.AutoComplete.TriggerShortcut;
            autoComplete.autoInsertSingleSuggestions = c.AutoComplete.AutoInsertSingleSuggestions;
            autoComplete.triggerDelay = std::chrono::milliseconds(c.AutoComplete.TriggerDelayMs);
            autoComplete.noSuggestionsLabel = c.AutoComplete.NoSuggestionsLabel;
            autoComplete.suggestionWidth = c.AutoComplete.SuggestionWidth;
            autoComplete.callback = [](TextEditor::AutoCompleteState &state) {
                ipte::AutoCompleteContext context;
                context.SearchTerm = state.searchTerm;
                context.Start = {state.searchTermStart.line, state.searchTermStart.index};
                context.End = {state.searchTermEnd.line, state.searchTermEnd.index};
                context.InIdentifier = state.inIdentifier;
                context.InNumber = state.inNumber;
                context.InComment = state.inComment;
                context.InString = state.inString;
                state.suggestions = provider ? provider(context) : std::vector<std::string>{};
            };
            upstream->SetAutoCompleteConfig(&autoComplete);
        }
        upstream->SetFindButtonLabel(c.FindButtonLabel);
        upstream->SetFindAllButtonLabel(c.FindAllButtonLabel);
        upstream->SetReplaceButtonLabel(c.ReplaceButtonLabel);
        upstream->SetReplaceAllButtonLabel(c.ReplaceAllButtonLabel);
    }
    upstream->SetText(setup.Text);
    if (const auto *lang = setup.Config.Language) {
        static const std::pair<const ipte::Language *, const TextEditor::Language *> languages[]{
            {ipte::C(), TextEditor::Language::C()}, {ipte::Cpp(), TextEditor::Language::Cpp()}, {ipte::Cs(), TextEditor::Language::Cs()}, {ipte::AngelScript(), TextEditor::Language::AngelScript()}, {ipte::Lua(), TextEditor::Language::Lua()}, {ipte::Python(), TextEditor::Language::Python()}, {ipte::Glsl(), TextEditor::Language::Glsl()}, {ipte::Hlsl(), TextEditor::Language::Hlsl()}, {ipte::Json(), TextEditor::Language::Json()}, {ipte::Markdown(), TextEditor::Language::Markdown()}, {ipte::Sql(), TextEditor::Language::Sql()}
        };
        for (const auto &[ours, theirs] : languages)
            if (ours == lang) upstream->SetLanguage(theirs);
    }
    if (setup.Upstream) setup.Upstream(*upstream);

    for (const auto &m : setup.Markers) upstream->AddMarker(m.Line, m.LineNumberColor, m.TextColor, m.LineNumberTooltip, m.TextTooltip);
    for (const auto &q : setup.Squiggles) upstream->AddSquiggle({q.Start.Line, q.Start.Index}, {q.End.Line, q.End.Index}, q.Type, q.Shade, q.Tooltip, q.Style == ipte::SquiggleStyle::Wave ? TextEditor::SquiggleStyle::wave : TextEditor::SquiggleStyle::background);

    ImGui::SetCurrentContext(ours.Ctx);
    auto state = ipte::Initial(setup.Text, setup.Config);
    for (const auto &m : setup.Markers) ipte::AddMarker(state, m.Line, m.LineNumberColor, m.TextColor, m.LineNumberTooltip, m.TextTooltip);
    for (const auto &q : setup.Squiggles) ipte::AddSquiggle(state, q.Start, q.End, q.Type, q.Shade, q.Tooltip, q.Style);

    int result = -1;
    Capture last;
    for (size_t f = 0; f < script.size(); ++f) {
        HarnessClock::Current += std::chrono::microseconds(int64_t(double(1.0f / 60) * 1e6));
        const auto a = theirs.Run(script[f], setup, [&] { upstream->Render("Editor"); });
        last = ours.Run(script[f], setup, [&] { state = ipte::Frame(std::move(state), "Editor"); });
        if (auto why = Differ(a, last); !why.empty()) {
            report = std::format("frame {}: {}", f, why);
            const auto dir = std::filesystem::path(IPTE_BINARY_DIR) / "mismatch" / name;
            WriteFrame(dir / std::format("frame{}-upstream.png", f), theirs.Ctx, a, setup.DisplaySize);
            WriteFrame(dir / std::format("frame{}-ours.png", f), ours.Ctx, last, setup.DisplaySize);
            result = int(f);
            break;
        }
    }
    if (result == -1 && std::getenv("IPTE_DUMP")) WriteFrame(std::filesystem::path(IPTE_BINARY_DIR) / "dump" / name / "last.png", ours.Ctx, last, setup.DisplaySize);
    upstream->~TextEditor();
    return result;
}
} // namespace harness
