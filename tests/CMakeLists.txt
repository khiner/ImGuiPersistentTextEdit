set(BOOST_UT_DISABLE_MODULE ON CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/ut ${CMAKE_BINARY_DIR}/lib/ut)

# Upstream, compiled from a copy with std::chrono::system_clock replaced by HarnessClock.
set(UPSTREAM_DIR ${CMAKE_SOURCE_DIR}/lib/ImGuiColorTextEdit)
set(UPSTREAM_COPY ${CMAKE_BINARY_DIR}/upstream/TextEditor.cpp)
file(READ ${UPSTREAM_DIR}/TextEditor.cpp UPSTREAM_SOURCE)
string(REPLACE "std::chrono::system_clock" "HarnessClock" UPSTREAM_SOURCE "${UPSTREAM_SOURCE}")
string(REPLACE "#include \"TextEditor.h\"" "#include \"HarnessClock.h\"\n#include \"TextEditor.h\"" UPSTREAM_SOURCE "${UPSTREAM_SOURCE}")
file(WRITE ${UPSTREAM_COPY} "${UPSTREAM_SOURCE}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${UPSTREAM_DIR}/TextEditor.cpp)

add_library(Upstream STATIC ${UPSTREAM_COPY})
target_include_directories(Upstream PUBLIC ${UPSTREAM_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(Upstream PRIVATE -O2 -w)
set_target_properties(Upstream PROPERTIES CXX_STANDARD 20)
target_link_libraries(Upstream PUBLIC imgui)

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS *.cpp)
add_executable(DrawListTest ${TEST_SOURCES})
target_compile_options(DrawListTest PRIVATE ${IPTE_WARNINGS})
target_compile_definitions(DrawListTest PRIVATE IPTE_BINARY_DIR="${CMAKE_BINARY_DIR}")
target_link_libraries(DrawListTest PRIVATE TextEdit Upstream Boost::ut)
