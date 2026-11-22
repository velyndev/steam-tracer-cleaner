# svg2rgba: the SVG rasterizer behind tools/make_icon.py. Standalone, not part of the app build.
#
# It compiles lunasvg (the SVG renderer Facet uses) from a source tree that is already on disk,
# so nothing is downloaded. Any configured app build tree has one under _deps:
#
#   cmake -S tools/icon -B build/icon-tool -G Ninja -DCMAKE_BUILD_TYPE=Release ^
#         -DLUNASVG_SOURCE_DIR=build/release/_deps/facet_lunasvg-src
#   cmake --build build/icon-tool
#
# Run both inside the MSVC developer environment (vcvars64.bat).

cmake_minimum_required(VERSION 3.20)
project(stc_icon_tool LANGUAGES C CXX)

set(LUNASVG_SOURCE_DIR "" CACHE PATH "A lunasvg 3.x source tree (for example <app build>/_deps/facet_lunasvg-src)")
if(NOT EXISTS "${LUNASVG_SOURCE_DIR}/include/lunasvg.h")
    message(FATAL_ERROR "Set LUNASVG_SOURCE_DIR to a lunasvg source tree; configure the app once and use "
                        "<app build>/_deps/facet_lunasvg-src.")
endif()

set(BUILD_SHARED_LIBS OFF)
set(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LUNASVG_DISABLE_LOAD_SYSTEM_FONTS ON CACHE BOOL "" FORCE)
add_subdirectory("${LUNASVG_SOURCE_DIR}" lunasvg EXCLUDE_FROM_ALL)

add_executable(svg2rgba svg2rgba.cpp)
target_compile_features(svg2rgba PRIVATE cxx_std_17)
target_link_libraries(svg2rgba PRIVATE lunasvg::lunasvg)
