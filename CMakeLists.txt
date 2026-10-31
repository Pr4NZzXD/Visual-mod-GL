cmake_minimum_required(VERSION 3.22)
project(visualmobile LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# And64InlineHook (arm64 inline hooks): two plain source files, compiled straight into the library
set(HOOK_DIR ${CMAKE_SOURCE_DIR}/external/And64InlineHook)
if(NOT EXISTS ${HOOK_DIR}/And64InlineHook.cpp OR NOT EXISTS ${HOOK_DIR}/And64InlineHook.hpp)
    message(FATAL_ERROR "And64InlineHook.cpp/.hpp not found in ${HOOK_DIR}")
endif()
set_source_files_properties(${HOOK_DIR}/And64InlineHook.cpp PROPERTIES COMPILE_FLAGS "-w")

# Dear ImGui, compiled straight into the library
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)

# The menu font: a TrueType font from ImGui's own misc/fonts folder, turned into a C array and compiled in,
# so nothing has to be shipped next to the library. If none of them is there, ImGui's built-in font is used.
set(UI_FONT_SOURCES "")
foreach(FONT_NAME Roboto-Medium.ttf DroidSans.ttf Karla-Regular.ttf Cousine-Regular.ttf)
    if(NOT UI_FONT_SOURCES AND EXISTS ${IMGUI_DIR}/misc/fonts/${FONT_NAME})
        file(READ ${IMGUI_DIR}/misc/fonts/${FONT_NAME} FONT_HEX HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," FONT_ARRAY "${FONT_HEX}")
        file(WRITE ${CMAKE_BINARY_DIR}/font_data.cpp
            "extern const unsigned char kUiFont[] = {${FONT_ARRAY}};\nextern const unsigned int kUiFontSize = sizeof(kUiFont);\n")
        set(UI_FONT_SOURCES ${CMAKE_BINARY_DIR}/font_data.cpp)
        message(STATUS "menu font: ${FONT_NAME}")
    endif()
endforeach()
if(NOT UI_FONT_SOURCES)
    message(STATUS "menu font: none found in ${IMGUI_DIR}/misc/fonts, using ImGui's built-in font")
endif()

add_library(visualmobile SHARED
    src/main.cpp
    src/log.cpp
    src/il2cpp.cpp
    src/shared.cpp
    src/effects.cpp
    src/fog.cpp
    src/perf.cpp
    src/scene.cpp
    src/esp.cpp
    src/cheat.cpp
    src/tas.cpp
    src/menu.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${HOOK_DIR}/And64InlineHook.cpp
    ${UI_FONT_SOURCES}
)

target_include_directories(visualmobile PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${HOOK_DIR}
)

target_compile_definitions(visualmobile PRIVATE
    IMGUI_IMPL_OPENGL_ES3
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_DEBUG_TOOLS
)

if(UI_FONT_SOURCES)
    target_compile_definitions(visualmobile PRIVATE HAVE_UI_FONT=1)
endif()

target_compile_options(visualmobile PRIVATE -O2 -fvisibility=hidden -Wno-unused-parameter)
target_link_options(visualmobile PRIVATE -Wl,--gc-sections -s)
target_link_libraries(visualmobile PRIVATE log android EGL GLESv3 dl)
