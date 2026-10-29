# ImGuiPersistentTextEdit

Persistent version of [ImGuiColorTextEdit](https://github.com/goossens/ImGuiColorTextEdit). Essentially ImGuiColorTextEdit + [ewig](https://github.com/arximboldi/ewig) but with my persistent data structures at [persistent](https://github.com/khiner/persistent).

The editor state is a value. The document is a persistent vector of codepoints with a newline count per trie node, so line lookups are logarithmic and edits share unchanged nodes. Cursors, settings, folds, markers and undo history are plain data. Input is converted to actions, `Update` applies an action to a state and returns the new state, and `Frame` draws from the state.

The only test compares draw data with upstream byte for byte after every frame of scripted interactions: typing, movement, selection, clipboard, undo, mouse, scrolling, find and replace, all eleven languages, bracket matching, word wrap in both line breaking modes, folding, minimaps, autocomplete, markers and squiggles, and a matrix of settings. Upstream behavior that is reproduced but wrong is listed in [UpstreamBugs.md](UpstreamBugs.md).

## Layout

- `src/TextEdit.h`: the public interface. `Settings`, `State`, `Initial`, `Frame`, marker and squiggle functions.
- `src/Update.*`: actions and `Update`. `src/Frame.cpp`: reads ImGui input into actions and draws, both in upstream's order.
- `src/Document.*`: document text functions. `src/Layout.*`: rows and columns after wrapping and folding. `src/Colorize.*`: colors and bracket pairs.
- `src/Unicode.cpp`, `src/Languages.cpp`, `src/LineBreak.cpp`: generated from upstream by `script/PortUpstream.py`. Unicode tables, language definitions with their re2c tokenizers, and Annex 14 line breaking. Rerun the script after updating `lib/ImGuiColorTextEdit`.
- `tests/Harness.*`: runs upstream and this editor in two headless ImGui contexts with the same input and compares the draw data. A mismatch writes both frames as PNGs under `build/mismatch/<scenario>/`. `IPTE_DUMP=1` writes the last frame of each passing scenario under `build/dump/`.
- `app/main.cpp`: demo on SDL3's GPU backend with a settings window and a window of persistent data stats.

The test build compiles a copy of upstream's source with `std::chrono::system_clock` replaced by a harness clock. The submodule is not modified.

## Build

Requires Homebrew LLVM and CMake 3.30+.

    cmake -B build -G Ninja && cmake --build build
    ./build/tests/DrawListTest
    ./build/app/ImGuiPersistentTextEdit [file]

`-DIPTE_APP=OFF` skips the SDL3 app, as CI does.
