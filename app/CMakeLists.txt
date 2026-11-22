add_executable(steam-tracer-cleaner WIN32
    win_main.cpp
    app_state.cpp
    task_runner.cpp
    capture.cpp
    fixture.cpp
    ${CMAKE_SOURCE_DIR}/app.rc
    # A .manifest source is merged into the manifest the linker embeds (/MANIFESTINPUT):
    # requireAdministrator, PerMonitorV2 DPI awareness, the UTF-8 code page, common controls v6.
    ${CMAKE_SOURCE_DIR}/app.manifest
)

# The resource compiler reads these through app.rc; without the dependency a changed icon or SVG
# would not rebuild the .res.
set_source_files_properties(${CMAKE_SOURCE_DIR}/app.rc PROPERTIES OBJECT_DEPENDS
    "${CMAKE_SOURCE_DIR}/assets/icon.ico;${CMAKE_SOURCE_DIR}/assets/icon.svg;${CMAKE_SOURCE_DIR}/assets/icon-small.svg"
)

target_include_directories(steam-tracer-cleaner PRIVATE
    ${CMAKE_SOURCE_DIR}
)

target_link_libraries(steam-tracer-cleaner PRIVATE
    stc_ui
    stc_core
    stc_platform
    facet::facet
    facet::imgui_backends
    stc_spdlog
    d3d11
    dxgi
    dwmapi
    Shcore
    Windowscodecs
    Ole32
)

stc_set_compile_options(steam-tracer-cleaner)

if(MSVC)
    # The UAC level comes from app.manifest; without this the linker adds its own asInvoker
    # trustInfo and the two conflict.
    target_link_options(steam-tracer-cleaner PRIVATE /MANIFESTUAC:NO)
endif()

set_target_properties(steam-tracer-cleaner PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    OUTPUT_NAME "steam-tracer-cleaner"
)
