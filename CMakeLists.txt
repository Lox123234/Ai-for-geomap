cmake_minimum_required(VERSION 3.21)
project(GeoMapAI VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.5)

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
)
target_include_directories(GeoMapCore PUBLIC app src/core src/import)
target_link_libraries(GeoMapCore PUBLIC Qt6::Core Qt6::Gui)

qt_add_executable(GeoMapAI
    app/main.cpp
    app/ThumbnailProvider.h
    app/ThumbnailProvider.cpp
)
target_compile_definitions(GeoMapAI PRIVATE GEOMAP_VERSION="${PROJECT_VERSION}")

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)

qt_add_qml_module(GeoMapAI
    URI GeoMapAI
    VERSION 1.0
    QML_FILES qml/Main.qml
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

        # On Windows the test executables are not deployed; let them find the Qt DLLs
        # (ENVIRONMENT_MODIFICATION needs CMake 3.22+; with 3.21 put <Qt>\bin on PATH).
        if(WIN32)
            set_tests_properties(GeoMapAITests GeoMapAIProjectTests GeoMapAIImportTests PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()
endif()
