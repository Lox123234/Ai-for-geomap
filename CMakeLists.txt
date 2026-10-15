cmake_minimum_required(VERSION 3.21)
# vcpkg (Windows): release-only DLLs built with the application's toolset, see triplets/.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET x64-windows-geomap CACHE STRING "vcpkg triplet")
    set(VCPKG_HOST_TRIPLET x64-windows-geomap CACHE STRING "vcpkg host triplet")
endif()
project(GeoMapAI VERSION 0.5.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.7 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2 ShaderTools)
qt_standard_project_setup(REQUIRES 6.5)

# GIS: GDAL/OGR and PROJ. Windows: vcpkg manifest (vcpkg.json) through the vcpkg toolchain;
# Linux: distribution packages (libgdal-dev, libproj-dev). See docs/BUILD.md.
find_package(GDAL 3.8 CONFIG QUIET)
if(NOT GDAL_FOUND)
    find_package(GDAL 3.8 REQUIRED)
endif()
find_package(PROJ CONFIG REQUIRED)
# Where the build finds proj.db and GDAL's data before deployment (deployed builds use
# <app>/share/proj and <app>/share/gdal).
if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
    set(GEOMAP_GIS_SHARE "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
else()
    set(GEOMAP_GIS_SHARE "/usr/share")
endif()

# Project, import and metadata logic without QML; shared by the app and the tests.
qt_add_library(GeoMapCore STATIC
    src/core/ProjectFormat.h
    src/import/ImportJob.h
    src/import/ImportJob.cpp
    src/import/ThumbnailCache.h
    src/import/ThumbnailCache.cpp
    app/ProjectManager.h
    app/ProjectManager.cpp
    app/PhotoModel.h
    app/PhotoModel.cpp
    app/ExifReader.h
    app/ExifReader.cpp
    src/processing/ProcessingTypes.h
    src/processing/ProcessingTypes.cpp
    src/processing/IProcessingEngine.h
    src/processing/GeoTiffInfo.h
    src/processing/GeoTiffInfo.cpp
    src/processing/Platform.h
    src/processing/Platform.cpp
    src/processing/Downloader.h
    src/processing/Downloader.cpp
    src/processing/OdmEngine.h
    src/processing/OdmEngine.cpp
    src/processing/ProcessingService.h
    src/processing/ProcessingService.cpp
    src/gis/GisEnvironment.h
    src/gis/GisEnvironment.cpp
    src/gis/Crs.h
    src/gis/Crs.cpp
    src/gis/Geodesy.h
    src/gis/Geodesy.cpp
    src/gis/RasterSource.h
    src/gis/RasterSource.cpp
    src/gis/VectorSource.h
    src/gis/VectorSource.cpp
    src/gis/TerrainAnalysis.h
    src/gis/TerrainAnalysis.cpp
    src/gis/ResultExport.h
    src/gis/ResultExport.cpp
    src/processing/ResultExporter.h
    src/processing/ResultExporter.cpp
    app/MapTiles.h
    app/MapTiles.cpp
    app/MapService.h
    app/MapService.cpp
    app/MapServiceAnalysis.cpp
    app/MapServiceEdit.cpp
    app/MapServiceTerrainEdit.cpp
    src/ui/commands/CommandRegistry.h
    src/ui/commands/CommandRegistry.cpp
    src/ui/workspace/WorkspaceState.h
    src/ui/workspace/WorkspaceState.cpp
    src/ui/workspace/AppLog.h
    src/ui/workspace/AppLog.cpp
    src/ui/workspace/AppInfo.h
    src/ui/workspace/AppInfo.cpp
    src/ui/workspace/ProjectTreeModel.h
    src/ui/workspace/ProjectTreeModel.cpp
    src/ui/workspace/Journal.h
    src/ui/workspace/Journal.cpp
    src/view3d/Scene3D.h
    src/view3d/Scene3D.cpp
    src/view3d/Camera3D.h
    src/view3d/Camera3D.cpp
    src/view3d/Scene3DService.h
    src/view3d/Scene3DService.cpp
    src/view3d/LasEdit.h
    src/view3d/LasEdit.cpp
    src/ai/ModelCard.h
    src/ai/ModelCard.cpp
    src/ai/SamSegmenter.h
    src/ai/SamSegmenter.cpp
    src/ai/MaskPolygons.h
    src/ai/MaskPolygons.cpp
    src/ai/AiService.h
    src/ai/AiService.cpp
    src/ai/OrtCommon.h
    src/ai/Inpainter.h
    src/ai/Inpainter.cpp
    src/gis/RasterEdit.h
    src/gis/RasterEdit.cpp
    src/processing/TrueOrthophoto.h
    src/processing/TrueOrthophoto.cpp
)
target_include_directories(GeoMapCore PUBLIC app src/core src/import src/processing src/gis src/ui/commands src/ui/workspace src/view3d src/ai)
target_link_libraries(GeoMapCore PUBLIC Qt6::Core Qt6::Gui Qt6::Network GDAL::GDAL PROJ::proj)
target_compile_definitions(GeoMapCore PRIVATE
    GEOMAP_PROJ_DATA="${GEOMAP_GIS_SHARE}/proj" GEOMAP_GDAL_DATA="${GEOMAP_GIS_SHARE}/gdal"
    GEOMAP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")   # documentation of developer builds

# Local AI: ONNX Runtime (MIT) from the official release archive, unpacked into
# GEOMAP_ONNXRUNTIME_DIR (see docs/BUILD.md). Without it the program builds and the AI tools
# say that this build has no AI runtime.
set(GEOMAP_ONNXRUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/onnxruntime-win-x64-1.30.0" CACHE PATH "ONNX Runtime release folder (include/, lib/)")
if(NOT "${GEOMAP_ONNXRUNTIME_DIR}" STREQUAL "${GEOMAP_ORT_SEARCHED_DIR}")   # another folder: search again
    unset(GEOMAP_ORT_INCLUDE CACHE)
    unset(GEOMAP_ORT_LIBRARY CACHE)
    set(GEOMAP_ORT_SEARCHED_DIR "${GEOMAP_ONNXRUNTIME_DIR}" CACHE INTERNAL "")
endif()
find_path(GEOMAP_ORT_INCLUDE onnxruntime_cxx_api.h PATHS "${GEOMAP_ONNXRUNTIME_DIR}/include" NO_DEFAULT_PATH)
find_library(GEOMAP_ORT_LIBRARY onnxruntime PATHS "${GEOMAP_ONNXRUNTIME_DIR}/lib" NO_DEFAULT_PATH)
if(GEOMAP_ORT_INCLUDE AND GEOMAP_ORT_LIBRARY)
    message(STATUS "ONNX Runtime: ${GEOMAP_ONNXRUNTIME_DIR}")
    target_include_directories(GeoMapCore PRIVATE "${GEOMAP_ORT_INCLUDE}")
    target_link_libraries(GeoMapCore PRIVATE "${GEOMAP_ORT_LIBRARY}")
    target_compile_definitions(GeoMapCore PRIVATE GEOMAP_HAS_ONNXRUNTIME)
    file(GLOB GEOMAP_ORT_RUNTIME "${GEOMAP_ONNXRUNTIME_DIR}/lib/*${CMAKE_SHARED_LIBRARY_SUFFIX}")
else()
    message(STATUS "ONNX Runtime not found in ${GEOMAP_ONNXRUNTIME_DIR}: building without local AI")
    set(GEOMAP_ORT_RUNTIME "")
endif()
# Model cards (models/<id>/model.json, licence) from the sources; the weights from
# GEOMAP_MODEL_WEIGHTS_DIR/<id> (downloaded by scripts/build-windows.ps1 -FetchAi, SHA-256 checked).
set(GEOMAP_MODEL_WEIGHTS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/models" CACHE PATH "Folder with <model id>/<weights> of the bundled AI models")
# The ONNX Runtime DLLs and the models go next to the executables (the app and the tests,
# which all import onnxruntime through GeoMapCore), before anything links.
get_property(geomap_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(geomap_multi_config)
    set(geomap_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>")
else()
    set(geomap_runtime_dir "${CMAKE_CURRENT_BINARY_DIR}")
endif()
set(geomap_ai_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${geomap_runtime_dir}")
foreach(dll IN LISTS GEOMAP_ORT_RUNTIME)
    list(APPEND geomap_ai_commands COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${geomap_runtime_dir}/")
endforeach()
file(GLOB geomap_cards RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}/models" CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/models/*/model.json")
foreach(card IN LISTS geomap_cards)
    get_filename_component(id "${card}" DIRECTORY)
    file(GLOB files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/models/${id}/*" "${GEOMAP_MODEL_WEIGHTS_DIR}/${id}/*.onnx")
    list(APPEND geomap_ai_commands COMMAND ${CMAKE_COMMAND} -E make_directory "${geomap_runtime_dir}/models/${id}"
                                   COMMAND ${CMAKE_COMMAND} -E copy_if_different ${files} "${geomap_runtime_dir}/models/${id}/")
endforeach()
add_custom_target(geomap_ai_files ${geomap_ai_commands} COMMENT "ONNX Runtime and AI models next to the executables" VERBATIM)
add_dependencies(GeoMapCore geomap_ai_files)
# Menus, toolbar and shortcuts: one list of commands (docs/UI_COMMANDS.md is generated from it).
qt_add_resources(GeoMapCore "geomap_commands" PREFIX "/geomap" BASE src/ui/commands FILES src/ui/commands/commands.json)

# The user interface as a QML module in a static library, shared by the application and the UI
# test. Every QML file and icon sits at the module root (qrc:/qt/qml/GeoMapAI/...).
set(GEOMAP_QML_FILES
    qml/Main.qml qml/Theme.qml qml/FlatButton.qml qml/SectionTitle.qml qml/ToolIconButton.qml
    qml/CommandMenuItem.qml qml/MainToolBar.qml qml/ProjectTreePanel.qml qml/PhotoListPanel.qml
    qml/PhotoInspector.qml qml/BottomPanel.qml qml/MapController.qml qml/MapLayersPanel.qml
    qml/MapLayerProperties.qml qml/MapCanvas.qml qml/ShortcutsDialog.qml qml/View3DWorkspace.qml qml/View3DPanel.qml qml/TerrainDialog.qml qml/MeasurementsPanel.qml qml/ExportDialog.qml
    qml/RasterCalcDialog.qml qml/AiModelsDialog.qml qml/MarkersDialog.qml qml/DemEditorDialog.qml qml/CloudEditorDialog.qml
    qml/ProjectSettingsDialog.qml qml/SaveAsDialog.qml qml/ProjectCheckDialog.qml)
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
foreach(file IN LISTS GEOMAP_QML_FILES)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS ${name})
endforeach()
file(GLOB GEOMAP_ICONS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} CONFIGURE_DEPENDS qml/icons/*.svg)
foreach(file IN LISTS GEOMAP_ICONS)
    get_filename_component(name ${file} NAME)
    set_source_files_properties(${file} PROPERTIES QT_RESOURCE_ALIAS icons/${name})
endforeach()

qt_add_library(GeoMapUi STATIC
    app/ThumbnailProvider.h
    app/ThumbnailProvider.cpp
    app/MapView.h
    app/MapView.cpp
    app/View3DItem.h
    app/View3DItem.cpp
)
# The module directory would otherwise be <build>/GeoMapAI, the path of the Linux executable.
qt_add_qml_module(GeoMapUi
    URI GeoMapAI
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/GeoMapAI
    QML_FILES ${GEOMAP_QML_FILES}
    RESOURCES ${GEOMAP_ICONS}
)
# 3D view shaders, compiled for every graphics API by qsb (:/qt/qml/GeoMapAI/shaders/*.qsb).
qt_add_shaders(GeoMapUi "geomap_shaders"
    PREFIX "/qt/qml/GeoMapAI"
    FILES shaders/points.vert shaders/points.frag shaders/mesh.vert shaders/mesh.frag
)
# QQuickRhiItem (Qt 6.7+) and QRhi are semi-public: Qt6::GuiPrivate.
target_link_libraries(GeoMapUi PUBLIC GeoMapCore Qt6::Core Qt6::Gui Qt6::GuiPrivate Qt6::Quick Qt6::QuickControls2)

qt_add_executable(GeoMapAI app/main.cpp)
if(WIN32)
    target_sources(GeoMapAI PRIVATE app/geomap.rc)   # icon of the executable
endif()
qt_add_resources(GeoMapAI "app_icon" PREFIX "/geomap" BASE app/resources FILES app/resources/geomap-256.png)
target_compile_definitions(GeoMapAI PRIVATE GEOMAP_VERSION="${PROJECT_VERSION}")
target_link_libraries(GeoMapAI PRIVATE GeoMapUi GeoMapUiplugin)

set_target_properties(GeoMapAI PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

install(TARGETS GeoMapAI BUNDLE DESTINATION . RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 COMPONENTS Test QUIET)
    if(Qt6Test_FOUND)
        qt_add_executable(GeoMapAITests
            tests/test_exif.cpp
            tests/TestImages.h
        )
        target_link_libraries(GeoMapAITests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAITests COMMAND GeoMapAITests)

        qt_add_executable(GeoMapAIProjectTests
            tests/test_project.cpp
            tests/TestImages.h
        )
        target_link_libraries(GeoMapAIProjectTests PRIVATE GeoMapCore Qt6::Qml Qt6::Test)
        add_test(NAME GeoMapAIProjectTests COMMAND GeoMapAIProjectTests)

        qt_add_executable(GeoMapAIImportTests
            tests/test_import.cpp
            tests/TestImages.h
        )
        target_link_libraries(GeoMapAIImportTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIImportTests COMMAND GeoMapAIImportTests)

        qt_add_executable(GeoMapAIProcessingTests
            tests/test_processing.cpp
            tests/MockEngine.h
            tests/TestImages.h
        )
        target_link_libraries(GeoMapAIProcessingTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIProcessingTests COMMAND GeoMapAIProcessingTests)
        qt_add_executable(GeoMapAIGisTests tests/test_gis.cpp)
        target_link_libraries(GeoMapAIGisTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIGisTests COMMAND GeoMapAIGisTests)
        # Terrain analysis on surfaces with known answers (slope, aspect, contours, profiles, volumes).
        qt_add_executable(GeoMapAITerrainTests tests/test_terrain.cpp)
        target_link_libraries(GeoMapAITerrainTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAITerrainTests COMMAND GeoMapAITerrainTests)
        # Export of results: rasters (CRS, cell size, COG), models with textures, the service.
        qt_add_executable(GeoMapAIExportTests tests/test_export.cpp tests/MockEngine.h)
        target_link_libraries(GeoMapAIExportTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIExportTests COMMAND GeoMapAIExportTests)
        # Local AI: model cards, masks to polygons, MobileSAM on a synthetic scene and the map
        # tool (the model parts are skipped when the build has no ONNX Runtime or no weights).
        qt_add_executable(GeoMapAIAiTests tests/test_ai.cpp)
        target_link_libraries(GeoMapAIAiTests PRIVATE GeoMapCore Qt6::Test)
        target_compile_definitions(GeoMapAIAiTests PRIVATE GEOMAP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME GeoMapAIAiTests COMMAND GeoMapAIAiTests)
        # True orthophoto: a synthetic scene with a known answer (GEOMAP_ORTHO_JOB=<ODM job> also
        # builds one from real results into GEOMAP_TEST_OUTPUT).
        qt_add_executable(GeoMapAIOrthophotoTests tests/test_orthophoto.cpp)
        target_link_libraries(GeoMapAIOrthophotoTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIOrthophotoTests COMMAND GeoMapAIOrthophotoTests)

        qt_add_executable(GeoMapAIWorkspaceTests tests/test_workspace.cpp tests/MockEngine.h)
        target_link_libraries(GeoMapAIWorkspaceTests PRIVATE GeoMapCore Qt6::Test)
        target_compile_definitions(GeoMapAIWorkspaceTests PRIVATE GEOMAP_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME GeoMapAIWorkspaceTests COMMAND GeoMapAIWorkspaceTests)

        # The real main window, offscreen with the software renderer: menus, commands, shortcuts,
        # panels and layout persistence (GEOMAP_TEST_OUTPUT=<folder> keeps screenshots).
        qt_add_executable(GeoMapAIUiTests tests/test_ui.cpp tests/MockEngine.h)
        target_link_libraries(GeoMapAIUiTests PRIVATE GeoMapUi GeoMapUiplugin Qt6::Test Qt6::Quick)
        add_test(NAME GeoMapAIUiTests COMMAND GeoMapAIUiTests)
        set_tests_properties(GeoMapAIUiTests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        # 3D: loaders (PLY, LAS, OBJ, DEM with drape), camera, picking, the scene service
        # (GEOMAP_3D_JOB=<ODM job folder> also checks real reconstruction results).
        qt_add_executable(GeoMapAIView3DTests tests/test_view3d.cpp)
        target_link_libraries(GeoMapAIView3DTests PRIVATE GeoMapCore Qt6::Test)
        add_test(NAME GeoMapAIView3DTests COMMAND GeoMapAIView3DTests)
        # The 3D view rendered by the GPU into a texture (no window on the screen); skipped
        # without a GPU API. GEOMAP_TEST_OUTPUT keeps the images.
        qt_add_executable(GeoMapAIView3DRenderTest tests/test_view3d_render.cpp)
        target_link_libraries(GeoMapAIView3DRenderTest PRIVATE GeoMapUi GeoMapUiplugin Qt6::Test Qt6::Quick Qt6::GuiPrivate)
        add_test(NAME GeoMapAIView3DRenderTest COMMAND GeoMapAIView3DRenderTest)
        set(geomap_tests GeoMapAITests GeoMapAIProjectTests GeoMapAIImportTests GeoMapAIProcessingTests GeoMapAIGisTests GeoMapAITerrainTests GeoMapAIExportTests
            GeoMapAIAiTests GeoMapAIOrthophotoTests GeoMapAIWorkspaceTests GeoMapAIUiTests GeoMapAIView3DTests GeoMapAIView3DRenderTest)

        # Real reconstruction with the built-in OpenDroneMap and a check of all its results;
        # minutes to an hour, so only when configured with -DGEOMAP_ODM_DATASET=<folder with
        # overlapping drone photos>. GEOMAP_ODM_HOME points at ODM when it is not in engines\odm.
        qt_add_executable(GeoMapAIOdmIntegrationTest tests/test_odm_integration.cpp)
        target_link_libraries(GeoMapAIOdmIntegrationTest PRIVATE GeoMapCore Qt6::Test)
        if(GEOMAP_ODM_DATASET)
            add_test(NAME GeoMapAIOdmIntegrationTest COMMAND GeoMapAIOdmIntegrationTest)
            set_tests_properties(GeoMapAIOdmIntegrationTest PROPERTIES
                ENVIRONMENT "GEOMAP_ODM_DATASET=${GEOMAP_ODM_DATASET};QTEST_FUNCTION_TIMEOUT=21600000" TIMEOUT 21600 LABELS integration)
            list(APPEND geomap_tests GeoMapAIOdmIntegrationTest)
        endif()

        # On Windows the test executables are not deployed; let them find the Qt DLLs
        # (ENVIRONMENT_MODIFICATION needs CMake 3.22+; with 3.21 put <Qt>\bin on PATH).
        # Without a console window Qt sends test output to the debugger; keep it in ctest logs.
        if(WIN32)
            set_tests_properties(${geomap_tests} PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>;QT_FORCE_STDERR_LOGGING=set:1")
        endif()
    endif()
endif()
