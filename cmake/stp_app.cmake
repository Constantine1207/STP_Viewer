option(STP_BUILD_TESTS "Build STP Viewer regression tests" ON)
include("${CMAKE_CURRENT_LIST_DIR}/stp_deps.cmake")
find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets OpenGLWidgets Test)
if(APPLE AND TARGET WrapOpenGL::WrapOpenGL)
    # Qt 6.8's finder adds legacy AGL even when absent from the current SDK.
    # The arm64 renderer uses Cocoa/OpenGL, never the removed Carbon AGL API.
    if(NOT EXISTS "${CMAKE_OSX_SYSROOT}/System/Library/Frameworks/AGL.framework")
        get_target_property(_stp_gl_links WrapOpenGL::WrapOpenGL INTERFACE_LINK_LIBRARIES)
        list(FILTER _stp_gl_links EXCLUDE REGEX "(AGL\\.framework|-framework AGL)")
        set_property(TARGET WrapOpenGL::WrapOpenGL PROPERTY INTERFACE_LINK_LIBRARIES "${_stp_gl_links}")
    endif()
endif()

add_library(stp_core STATIC
    src/stp/cad_document.cpp src/stp/bom_export.cpp src/stp/part_export.cpp src/stp/pmi.cpp
    src/stp/measurement.cpp)
target_include_directories(stp_core PUBLIC src src/stp "${OpenCASCADE_INCLUDE_DIR}")
target_link_libraries(stp_core PUBLIC Qt6::Core Qt6::Gui STP::xlsxwriter
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKMesh
    TKShHealing TKService TKV3d TKOpenGl TKCDF TKLCAF TKCAF TKVCAF TKXCAF TKXSBase TKDESTEP)
target_compile_features(stp_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(stp_core PUBLIC /utf-8)
    target_compile_definitions(stp_core PUBLIC NOMINMAX _USE_MATH_DEFINES)
endif()

add_library(stp_view STATIC
    src/stp/viewer_widget.cpp src/stp/viewer_widget.h
    src/stp/measurement_panel.cpp src/stp/measurement_panel.h
    src/stp/cad_job_queue.cpp src/stp/cad_job_queue.h
    src/app/widget_occ_view.cpp src/app/widget_occ_view_impl.cpp
    src/app/qtopengl_utils.cpp src/app/occt_window.cpp)
target_link_libraries(stp_view PUBLIC stp_core MayoCoreLib Qt6::Widgets Qt6::OpenGLWidgets)
target_include_directories(stp_view PRIVATE src "${CMAKE_BINARY_DIR}")
set_target_properties(stp_view PROPERTIES AUTOMOC ON)
add_executable(STPViewer MACOSX_BUNDLE WIN32
    src/stp/main.cpp src/stp/main_window.cpp src/stp/main_window.h
    src/stp/document_workspace.cpp src/stp/document_workspace.h)
target_link_libraries(STPViewer PRIVATE stp_view)
target_include_directories(STPViewer PRIVATE src "${CMAKE_BINARY_DIR}")
target_compile_definitions(STPViewer PRIVATE
    STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources"
    STP_TRANSLATION_DIR="${CMAKE_SOURCE_DIR}/resources/translations")
set_target_properties(STPViewer PROPERTIES
    AUTOMOC ON
    MACOSX_BUNDLE_BUNDLE_NAME "STP Viewer"
    MACOSX_BUNDLE_GUI_IDENTIFIER "local.stpviewer.desktop"
    MACOSX_BUNDLE_BUNDLE_VERSION "1.0.0"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "1.0.0")
if(APPLE)
    set(_stp_app_icon "${PROJECT_SOURCE_DIR}/images/stpviewer-aeme.icns")
    set_source_files_properties("${_stp_app_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
    target_sources(STPViewer PRIVATE "${_stp_app_icon}")
    target_sources(STPViewer PRIVATE resources/stp_grab_icons.qrc)
    set_target_properties(STPViewer PROPERTIES AUTORCC ON)
    set_target_properties(STPViewer PROPERTIES MACOSX_BUNDLE_ICON_FILE "stpviewer-aeme.icns")
    enable_language(OBJCXX)
    target_sources(stp_view PRIVATE src/stp/mac_accessibility.mm src/stp/mac_accessibility.h)
    target_link_libraries(stp_view PRIVATE Qt6::GuiPrivate "-framework AppKit")
    target_compile_definitions(STPViewer PRIVATE GL_SILENCE_DEPRECATION)
    target_compile_definitions(stp_view PRIVATE GL_SILENCE_DEPRECATION)
endif()
if(WIN32)
    enable_language(RC)
    target_sources(STPViewer PRIVATE images/stpviewer-aeme.rc resources/stp_icon.qrc resources/stp_grab_icons.qrc)
    set_target_properties(STPViewer PROPERTIES AUTORCC ON)
    target_include_directories(STPViewer PRIVATE images)
endif()
if(STP_BUILD_TESTS)
    enable_testing()
    add_executable(stp_core_tests tests/stp_core_tests.cpp)
    target_link_libraries(stp_core_tests PRIVATE stp_core)
    add_test(NAME stp_core_tests COMMAND stp_core_tests)
    add_executable(stp_measurement_tests tests/stp_measurement_tests.cpp)
    target_link_libraries(stp_measurement_tests PRIVATE stp_core)
    add_test(NAME stp_measurement_tests COMMAND stp_measurement_tests)
    add_executable(stp_pmi_tests tests/stp_pmi_tests.cpp)
    target_link_libraries(stp_pmi_tests PRIVATE stp_core Qt6::Widgets)
    add_test(NAME stp_pmi_tests COMMAND stp_pmi_tests)
    set_tests_properties(stp_pmi_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    # Requires an interactive desktop and a working OpenGL driver. Build on demand;
    # headless CTest continues to exercise the CAD/data services independently.
    add_executable(stp_viewer_tests EXCLUDE_FROM_ALL tests/stp_viewer_tests.cpp)
    target_link_libraries(stp_viewer_tests PRIVATE stp_view Qt6::Test)
    target_compile_definitions(stp_viewer_tests PRIVATE
        STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources")
    add_executable(stp_job_queue_tests tests/stp_job_queue_tests.cpp)
    target_link_libraries(stp_job_queue_tests PRIVATE stp_view)
    set_target_properties(stp_job_queue_tests PROPERTIES AUTOMOC ON)
    add_test(NAME stp_job_queue_tests COMMAND stp_job_queue_tests)
    add_executable(stp_workspace_tests EXCLUDE_FROM_ALL
        tests/stp_workspace_tests.cpp src/stp/document_workspace.cpp src/stp/document_workspace.h
        src/stp/main_window.cpp src/stp/main_window.h)
    target_link_libraries(stp_workspace_tests PRIVATE stp_view Qt6::Test)
    target_include_directories(stp_workspace_tests PRIVATE src "${CMAKE_BINARY_DIR}")
    target_compile_definitions(stp_workspace_tests PRIVATE
        STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources")
    set_target_properties(stp_workspace_tests PROPERTIES AUTOMOC ON)
    add_executable(stp_appearance_tests EXCLUDE_FROM_ALL
        tests/stp_appearance_tests.cpp src/stp/document_workspace.cpp src/stp/document_workspace.h
        src/stp/main_window.cpp src/stp/main_window.h)
    target_link_libraries(stp_appearance_tests PRIVATE stp_view Qt6::Test)
    target_include_directories(stp_appearance_tests PRIVATE src "${CMAKE_BINARY_DIR}")
    target_compile_definitions(stp_appearance_tests PRIVATE
        STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources")
    set_target_properties(stp_appearance_tests PROPERTIES AUTOMOC ON)
    if(APPLE)
        set(_stp_layout_target stp_mac_layout_tests)
    else()
        set(_stp_layout_target stp_layout_tests)
    endif()
    add_executable(${_stp_layout_target} EXCLUDE_FROM_ALL
        tests/stp_mac_layout_tests.cpp src/stp/main_window.cpp src/stp/main_window.h)
    target_link_libraries(${_stp_layout_target} PRIVATE stp_view Qt6::Test)
    target_include_directories(${_stp_layout_target} PRIVATE src "${CMAKE_BINARY_DIR}")
    target_compile_definitions(${_stp_layout_target} PRIVATE
        STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources")
    set_target_properties(${_stp_layout_target} PROPERTIES AUTOMOC ON)
    if(APPLE OR WIN32)
        foreach(_stp_grab_target stp_workspace_tests stp_appearance_tests ${_stp_layout_target})
            target_sources(${_stp_grab_target} PRIVATE resources/stp_grab_icons.qrc)
            set_target_properties(${_stp_grab_target} PROPERTIES AUTORCC ON)
        endforeach()
        if(APPLE)
            add_executable(stp_mac_accessibility_tests EXCLUDE_FROM_ALL tests/stp_mac_accessibility_tests.mm)
            target_link_libraries(stp_mac_accessibility_tests PRIVATE stp_view Qt6::Test Qt6::GuiPrivate "-framework AppKit")
        endif()
        add_executable(stp_measurement_integration_tests EXCLUDE_FROM_ALL
            tests/stp_measurement_integration_tests.cpp src/stp/document_workspace.cpp src/stp/document_workspace.h
            src/stp/main_window.cpp src/stp/main_window.h resources/stp_grab_icons.qrc)
        target_link_libraries(stp_measurement_integration_tests PRIVATE stp_view Qt6::Test)
        target_include_directories(stp_measurement_integration_tests PRIVATE src "${CMAKE_BINARY_DIR}")
        target_compile_definitions(stp_measurement_integration_tests PRIVATE
            STP_OCCT_RESOURCE_DIR="${STP_DEPS_PREFIX}/share/opencascade/resources")
        set_target_properties(stp_measurement_integration_tests PROPERTIES AUTOMOC ON AUTORCC ON)
    endif()
endif()
