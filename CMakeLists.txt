cmake_minimum_required(VERSION 3.21)
# vcpkg (Windows): release-only DLLs built with the application's toolset, see triplets/.
if(CMAKE_HOST_WIN32 AND NOT DEFINED VCPKG_TARGET_TRIPLET)
    set(VCPKG_TARGET_TRIPLET x64-windows-geomap CACHE STRING "vcpkg triplet")
    set(VCPKG_HOST_TRIPLET x64-windows-geomap CACHE STRING "vcpkg host triplet")
endif()
project(GeoMapAI VERSION 0.3.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Network Qml Quick QuickControls2)
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
    app/MapTiles.h
    app/MapTiles.cpp
    app/MapService.h
    app/MapService.cpp
)
target_include_directories(GeoMapCore PUBLIC app src/core src/import src/processing src/gis)
target_link_libraries(GeoMapCore PUBLIC Qt6::Core Qt6::Gui Qt6::Network GDAL::GDAL PROJ::proj)
target_compile_definitions(GeoMapCore PRIVATE
    GEOMAP_PROJ_DATA="${GEOMAP_GIS_SHARE}/proj" GEOMAP_GDAL_DATA="${GEOMAP_GIS_SHARE}/gdal")

qt_add_executable(GeoMapAI
    app/main.cpp
    app/ThumbnailProvider.h
    app/ThumbnailProvider.cpp
    app/MapView.h
    app/MapView.cpp
)
target_compile_definitions(GeoMapAI PRIVATE GEOMAP_VERSION="${PROJECT_VERSION}")

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)

# The module directory would otherwise be <build>/GeoMapAI, the path of the Linux executable.
qt_add_qml_module(GeoMapAI
    URI GeoMapAI
    VERSION 1.0
    OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/qml/GeoMapAI
    QML_FILES qml/Main.qml qml/MapWorkspace.qml qml/FlatButton.qml qml/SectionTitle.qml
)

target_link_libraries(GeoMapAI PRIVATE
    GeoMapCore Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2
)

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
        set(geomap_tests GeoMapAITests GeoMapAIProjectTests GeoMapAIImportTests GeoMapAIProcessingTests GeoMapAIGisTests)

        # Real reconstruction with Docker and the pinned ODM image; minutes to hours, so only
        # when configured with -DGEOMAP_ODM_DATASET=<folder with overlapping drone photos>.
        qt_add_executable(GeoMapAIOdmIntegrationTest tests/test_odm_integration.cpp)
        target_link_libraries(GeoMapAIOdmIntegrationTest PRIVATE GeoMapCore Qt6::Test)
        if(GEOMAP_ODM_DATASET)
            add_test(NAME GeoMapAIOdmIntegrationTest COMMAND GeoMapAIOdmIntegrationTest)
            set_tests_properties(GeoMapAIOdmIntegrationTest PROPERTIES
                ENVIRONMENT "GEOMAP_ODM_DATASET=${GEOMAP_ODM_DATASET}" TIMEOUT 7200 LABELS integration)
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
