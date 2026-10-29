cmake_minimum_required(VERSION 3.30)

include(cmake/Compiler.cmake)
project(ImGuiPersistentTextEdit LANGUAGES C CXX)

option(IPTE_APP "Build the SDL3 demo app" ON)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Dear ImGui, the version and definitions of upstream's example.
add_library(imgui STATIC
    lib/imgui/imgui.cpp lib/imgui/imgui_draw.cpp lib/imgui/imgui_tables.cpp lib/imgui/imgui_widgets.cpp lib/imgui/imgui_demo.cpp)
target_include_directories(imgui SYSTEM PUBLIC lib/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_USE_WCHAR32 IMGUI_DEFINE_MATH_OPERATORS IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_OBSOLETE_KEYIO)
target_compile_options(imgui PRIVATE -O2 -w)

# persistent, compiled from its sources directly to skip its tests and immer.
add_library(persistent STATIC lib/persistent/src/Vector.cpp lib/persistent/src/Hamt.cpp)
target_include_directories(persistent SYSTEM PUBLIC lib/persistent/src)
target_compile_definitions(persistent PUBLIC VECTOR_WEIGHT_KEY=10) # Newlines, for line lookups.
target_compile_options(persistent PRIVATE -O2 -w)
set_target_properties(persistent PROPERTIES CXX_STANDARD 23 INTERPROCEDURAL_OPTIMIZATION ON)

file(GLOB TEXTEDIT_SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(TextEdit STATIC ${TEXTEDIT_SOURCES})
target_include_directories(TextEdit PUBLIC src)
target_compile_options(TextEdit PRIVATE -Wall -Wextra -Wno-missing-field-initializers -Wno-missing-designated-field-initializers -fno-rtti)
target_link_libraries(TextEdit PUBLIC imgui persistent)

add_subdirectory(tests)
if(IPTE_APP)
    add_subdirectory(app)
endif()
