#include "Language.h"
#include "TextEdit.h"
#include "Vector.h"

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlgpu3.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

namespace {
constexpr const char *SampleText = R"(#include <stdio.h>

/* A persistent text editor: every keystroke is a new value of the document,
   sharing all but a few nodes with the one before it. */
int main(int argc, char **argv) {
	const char *greeting = "hello, world";
	for (int i = 0; i < argc; i++) {
		printf("%s %s\n", greeting, argv[i]);
	}
	return 0;
}
)";

struct App {
    ipte::State Editor;
    bool ShowSettings{true}, ShowPersistent{true};
    int LanguageIndex{1};
    float FontSize{17.0f};
};

const ipte::Language *Languages[]{nullptr, ipte::Cpp(), ipte::C(), ipte::Cs(), ipte::AngelScript(), ipte::Lua(), ipte::Python(), ipte::Glsl(), ipte::Hlsl(), ipte::Json(), ipte::Markdown(), ipte::Sql()};

// Begins the window `name` while `*open`, placed at `pos` with `width` on first use. Call `ImGui::End` when it returns true.
bool BeginToolWindow(const char *name, bool *open, ImVec2 pos, float width) {
    if (!*open) return false;
    ImGui::SetNextWindowPos(pos, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({width, 0}, ImGuiCond_FirstUseEver);
    if (ImGui::Begin(name, open)) return true;
    ImGui::End();
    return false;
}

void SettingsWindow(App &app) {
    if (!BeginToolWindow("Settings", &app.ShowSettings, {910, 10}, 320)) return;
    auto &c = app.Editor.Config;
    if (ImGui::Combo("Language", &app.LanguageIndex, [](void *, int i) { return i ? Languages[i]->Name.c_str() : "None"; }, nullptr, IM_ARRAYSIZE(Languages))) c.Language = Languages[app.LanguageIndex];
    int tabSize{int(c.TabSize)};
    if (ImGui::SliderInt("Tab size", &tabSize, 1, 8)) c.TabSize = uint32_t(tabSize);
    ImGui::SliderFloat("Line spacing", &c.LineSpacing, 1.0f, 2.0f);
    ImGui::SliderFloat("Font size", &app.FontSize, 8.0f, 32.0f);
    bool dark{c.Colors == ipte::DarkPalette()};
    if (ImGui::Checkbox("Dark palette", &dark)) {
        c.Colors = dark ? ipte::DarkPalette() : ipte::LightPalette();
        app.Editor.ShadedAlpha = -1;
        if (dark) ImGui::StyleColorsDark();
        else ImGui::StyleColorsLight();
    }
    ImGui::Checkbox("Insert spaces on tabs", &c.InsertSpacesOnTabs);
    ImGui::Checkbox("Word wrap", &c.WordWrap);
    ImGui::Checkbox("Unicode line breaking", &c.LineBreak.UseUnicodeAnnex14);
    ImGui::Checkbox("Read only", &c.ReadOnly);
    ImGui::Checkbox("Carets visible", &c.CaretsVisible);
    ImGui::Checkbox("Auto indent", &c.AutoIndent);
    ImGui::Checkbox("Show spaces", &c.ShowSpaces);
    ImGui::Checkbox("Show tabs", &c.ShowTabs);
    ImGui::Checkbox("Show line numbers", &c.ShowLineNumbers);
    ImGui::Checkbox("Show minimap", &c.ShowMiniMap);
    ImGui::Checkbox("Show scrollbar minimap", &c.ShowScrollbarMiniMap);
    ImGui::Checkbox("Show current line highlight", &c.ShowCurrentLineHighlight);
    if (ImGui::Checkbox("Show matching brackets", &c.ShowMatchingBrackets) && !c.ShowMatchingBrackets) c.LineFolding = false;
    ImGui::Checkbox("Complete paired glyphs", &c.CompletePairedGlyphs);
    if (ImGui::Checkbox("Line folding", &c.LineFolding) && c.LineFolding) c.ShowMatchingBrackets = true;
    ImGui::Checkbox("Overwrite", &c.Overwrite);
    ImGui::Checkbox("Middle mouse pans", &c.PanMode);
    ImGui::Checkbox("Show pan/scroll indicator", &c.ShowPanScrollIndicator);
    ImGui::Checkbox("Find and replace", &c.FindReplaceEnabled);
    ImGui::Checkbox("Autocomplete", &c.AutoComplete.Enabled);
    ImGui::End();
}

// Placeholder autocomplete provider.
std::vector<std::string> Suggest(const ipte::AutoCompleteContext &context) { return context.SearchTerm.empty() ? std::vector<std::string>{} : std::vector<std::string>{context.SearchTerm + "_suggestion"}; }

void PersistentWindow(App &app) {
    if (!BeginToolWindow("Persistent", &app.ShowPersistent, {910, 600}, 360)) return;
    const auto &s = app.Editor;
    ImGui::Text("Codepoints: %llu", (unsigned long long)s.Doc.Size);
    ImGui::Text("Lines: %llu", (unsigned long long)(vec::Weight(s.Doc) + 1));
    ImGui::Text("History: %zu steps, at %zu", s.History.size(), s.UndoIndex);
    ImGui::Text("Cursors: %zu", s.Cursors.Items.size());
    if (const auto held = vec::Held()) {
        ImGui::Separator();
        ImGui::TextUnformatted("Trie nodes, over every version the history keeps:");
        ImGui::Text("Live: %.1f KB", double(held->LiveBytes) / 1024);
        ImGui::Text("Spanned: %.1f KB", double(held->SpannedBytes) / 1024);
        ImGui::Text("Reserved: %.1f KB", double(held->ReservedBytes) / 1024);
    }
    ImGui::End();
}
} // namespace

int main(int argc, char **argv) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    auto *window = SDL_CreateWindow("ImGuiPersistentTextEdit", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    auto *device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_METALLIB | SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_DXIL, true, nullptr);
    if (!window || !device || !SDL_ClaimWindowForGPUDevice(device, window)) {
        std::fprintf(stderr, "SDL GPU: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC);

    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLGPU(window);
    ImGui_ImplSDLGPU3_InitInfo init{.Device = device, .ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window)};
    ImGui_ImplSDLGPU3_Init(&init);

    App app;
    std::string text{SampleText};
    if (argc > 1) {
        std::ifstream file(argv[1]);
        text.assign(std::istreambuf_iterator<char>(file), {});
    }
    app.Editor = ipte::Initial(text, {.Language = Languages[app.LanguageIndex], .AutoComplete = {.Provider = Suggest}});
    app.Editor.FocusOnEditor = true;

    bool done{false};
    while (!done) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT) done = true;
            if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) done = true;
        }
        if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
            SDL_Delay(10);
            continue;
        }
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::PushFont(nullptr, app.FontSize);
        ImGui::SetNextWindowPos({0, 0}, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize({900, 800}, ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Editor", nullptr, ImGuiWindowFlags_MenuBar)) {
            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu("View")) {
                    ImGui::MenuItem("Settings", nullptr, &app.ShowSettings);
                    ImGui::MenuItem("Persistent", nullptr, &app.ShowPersistent);
                    ImGui::EndMenu();
                }
                ImGui::EndMenuBar();
            }
            app.Editor = ipte::Frame(std::move(app.Editor), "TextEdit");
        }
        ImGui::End();
        ImGui::PopFont();
        SettingsWindow(app);
        PersistentWindow(app);

        ImGui::Render();
        auto *drawData = ImGui::GetDrawData();
        auto *commandBuffer = SDL_AcquireGPUCommandBuffer(device);
        SDL_GPUTexture *swapchain{nullptr};
        SDL_WaitAndAcquireGPUSwapchainTexture(commandBuffer, window, &swapchain, nullptr, nullptr);
        if (swapchain && drawData->DisplaySize.x > 0 && drawData->DisplaySize.y > 0) {
            ImGui_ImplSDLGPU3_PrepareDrawData(drawData, commandBuffer);
            SDL_GPUColorTargetInfo target{.texture = swapchain, .clear_color = {0.1f, 0.1f, 0.1f, 1.0f}, .load_op = SDL_GPU_LOADOP_CLEAR, .store_op = SDL_GPU_STOREOP_STORE};
            auto *pass = SDL_BeginGPURenderPass(commandBuffer, &target, 1, nullptr);
            ImGui_ImplSDLGPU3_RenderDrawData(drawData, commandBuffer, pass);
            SDL_EndGPURenderPass(pass);
        }
        SDL_SubmitGPUCommandBuffer(commandBuffer);
    }

    SDL_WaitForGPUIdle(device);
    ImGui_ImplSDL3_Shutdown();
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui::DestroyContext();
    SDL_ReleaseWindowFromGPUDevice(device, window);
    SDL_DestroyGPUDevice(device);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
