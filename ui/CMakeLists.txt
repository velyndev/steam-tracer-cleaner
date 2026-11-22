add_library(stc_ui STATIC
    app_shell.cpp
    appearance.cpp
    assets.cpp
    common.cpp
    pages/cleanup_page.cpp
    pages/cleanup_jobs.cpp
    pages/cleanup_dev_states.cpp
    pages/cleanup_summary.cpp
    pages/cleanup_targets.cpp
    pages/cleanup_details.cpp
    pages/backups_page.cpp
    pages/exclusions_page.cpp
    pages/audit_page.cpp
    pages/configs_page.cpp
    pages/settings_page.cpp
    pages/settings_widgets.cpp
)

target_include_directories(stc_ui PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(stc_ui PUBLIC
    stc_core
    stc_platform
    facet::facet
    stc_spdlog
    Ole32
    Shell32
)

stc_set_compile_options(stc_ui)
