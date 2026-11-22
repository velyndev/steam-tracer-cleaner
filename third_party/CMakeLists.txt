# Third-party libraries. spdlog, nlohmann/json, doctest and SQLite are vendored here; Facet is
# fetched (below) and fetches Dear ImGui, FreeType and lunasvg itself.

# --- Facet -------------------------------------------------------------------------------------
# The UI framework, pinned to one commit. To build against a local checkout instead of the
# download, configure with
#   -DFETCHCONTENT_SOURCE_DIR_FACET=C:/path/to/imgui-design-framework
# (or put that cache variable in a CMakeUserPresets.json). The local checkout should be at the
# pinned commit, otherwise the build is not the one CI makes.
#
# Facet then fetches Dear ImGui v1.93.0 (with FreeType) and lunasvg. Dear ImGui is its target
# facet::imgui, which defines IMGUI_DISABLE_OBSOLETE_FUNCTIONS publicly; the Win32 and DX11
# backends are facet::imgui_backends.
include(FetchContent)

# Normal variables: Facet's option() calls take them (CMP0077 is NEW in Facet's scope).
set(FACET_BUILD_DEMO OFF)
set(FACET_BUILD_TESTS OFF)
set(FACET_BUILD_CONSUMER_TEST OFF)

FetchContent_Declare(facet
    GIT_REPOSITORY https://github.com/velyndev/imgui-facet.git
    GIT_TAG        6efdc2274976500aad0694d1970aef38c808e640
)
FetchContent_MakeAvailable(facet)

# --- Vendored ----------------------------------------------------------------------------------

# spdlog. Header-only, with std::format used so we don't drag fmt in.
add_library(stc_spdlog INTERFACE)
target_include_directories(stc_spdlog INTERFACE ${CMAKE_CURRENT_LIST_DIR}/spdlog/include)
target_compile_definitions(stc_spdlog INTERFACE SPDLOG_HEADER_ONLY SPDLOG_USE_STD_FORMAT)

# nlohmann::json - single-header
add_library(stc_json INTERFACE)
target_include_directories(stc_json INTERFACE ${CMAKE_CURRENT_LIST_DIR}/nlohmann_json/single_include)

# doctest - single-header testing framework
add_library(stc_doctest INTERFACE)
target_include_directories(stc_doctest INTERFACE ${CMAKE_CURRENT_LIST_DIR}/doctest)

# SQLite amalgamation. Compiled as C, warnings off, threadsafe enabled, no dl/load extension.
add_library(stc_sqlite STATIC sqlite/sqlite3.c)
target_include_directories(stc_sqlite PUBLIC ${CMAKE_CURRENT_LIST_DIR}/sqlite)
target_compile_definitions(stc_sqlite PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
)
if(MSVC)
    target_compile_options(stc_sqlite PRIVATE /W0)
endif()
