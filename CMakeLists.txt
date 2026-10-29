cmake_minimum_required(VERSION 3.22)
project(visualmobile LANGUAGES C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Dobby (inline hooks), built as a static library
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(Plugin.SymbolResolver OFF CACHE BOOL "" FORCE)
add_subdirectory(external/Dobby EXCLUDE_FROM_ALL)

# Dear ImGui, compiled straight into the library
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/external/imgui)

add_library(visualmobile SHARED
    src/main.cpp
    src/log.cpp
    src/il2cpp.cpp
    src/shared.cpp
    src/effects.cpp
    src/menu.cpp
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(visualmobile PRIVATE
    src
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${CMAKE_SOURCE_DIR}/external/Dobby/include
)

target_compile_definitions(visualmobile PRIVATE
    IMGUI_IMPL_OPENGL_ES3
    IMGUI_DISABLE_DEMO_WINDOWS
    IMGUI_DISABLE_DEBUG_TOOLS
)

target_compile_options(visualmobile PRIVATE -O2 -fvisibility=hidden -Wno-unused-parameter)
target_link_options(visualmobile PRIVATE -Wl,--gc-sections -s)
target_link_libraries(visualmobile PRIVATE dobby log android EGL GLESv3 dl)
