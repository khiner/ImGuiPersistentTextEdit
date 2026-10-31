include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
FetchContent_Declare(sdl GIT_REPOSITORY https://github.com/libsdl-org/SDL GIT_TAG release-3.4.18 GIT_SHALLOW ON)
FetchContent_MakeAvailable(sdl)

add_library(imgui_sdl3 STATIC
    ${CMAKE_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdl3.cpp
    ${CMAKE_SOURCE_DIR}/lib/imgui/backends/imgui_impl_sdlgpu3.cpp)
target_include_directories(imgui_sdl3 SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/lib/imgui/backends)
target_compile_options(imgui_sdl3 PRIVATE -O2 -w)
target_link_libraries(imgui_sdl3 PUBLIC imgui SDL3::SDL3-static)

file(GLOB APP_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(ImGuiPersistentTextEdit ${APP_SOURCES})
target_compile_options(ImGuiPersistentTextEdit PRIVATE ${IPTE_WARNINGS})
target_link_libraries(ImGuiPersistentTextEdit PRIVATE TextEdit imgui_sdl3)
