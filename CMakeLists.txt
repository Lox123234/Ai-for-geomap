cmake_minimum_required(VERSION 3.21)
project(GeoMapAI VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(GeoMapAI
    app/main.cpp
    app/ProjectManager.h
    app/ProjectManager.cpp
    app/PhotoModel.h
    app/PhotoModel.cpp
    app/ExifReader.h
    app/ExifReader.cpp
)

set_source_files_properties(qml/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)

qt_add_qml_module(GeoMapAI
    URI GeoMapAI
    VERSION 1.0
    QML_FILES qml/Main.qml
)

target_link_libraries(GeoMapAI PRIVATE
    Qt6::Core Qt6::Gui Qt6::Quick Qt6::QuickControls2
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
            app/ExifReader.h
            app/ExifReader.cpp
        )
        target_link_libraries(GeoMapAITests PRIVATE Qt6::Core Qt6::Gui Qt6::Test)
        add_test(NAME GeoMapAITests COMMAND GeoMapAITests)

        qt_add_executable(GeoMapAIProjectTests
            tests/test_project.cpp
            tests/TestImages.h
            app/ProjectManager.h
            app/ProjectManager.cpp
            app/PhotoModel.h
            app/PhotoModel.cpp
            app/ExifReader.h
            app/ExifReader.cpp
        )
        target_link_libraries(GeoMapAIProjectTests PRIVATE Qt6::Core Qt6::Gui Qt6::Qml Qt6::Test)
        add_test(NAME GeoMapAIProjectTests COMMAND GeoMapAIProjectTests)

        # On Windows the test executables are not deployed; let them find the Qt DLLs
        # (ENVIRONMENT_MODIFICATION needs CMake 3.22+; with 3.21 put <Qt>\bin on PATH).
        if(WIN32)
            set_tests_properties(GeoMapAITests GeoMapAIProjectTests PROPERTIES
                ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>")
        endif()
    endif()
endif()
