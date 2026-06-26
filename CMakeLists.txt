cmake_minimum_required(VERSION 3.20)
project(lwepaper VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Qml Quick)
find_package(X11 REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(XCOMPOSITE REQUIRED IMPORTED_TARGET xcomposite)

qt_standard_project_setup(REQUIRES 6.5)

qt_add_qml_module(lwepaperplugin
    URI local.lwepaper
    VERSION 1.0
    SOURCES
        src/lweview.h
        src/lweview.cpp
        src/lwelibrary.h
        src/lwelibrary.cpp
        src/lwescenepatch.h
        src/lwescenepatch.cpp
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/qml/local/lwepaper
)

target_include_directories(lwepaperplugin PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(lwepaperplugin PRIVATE
    Qt6::Core Qt6::Gui Qt6::Qml Qt6::Quick
    X11::X11
    X11::Xtst
    PkgConfig::XCOMPOSITE
)

# Install QML plugin under the user-local Qt QML path so plasmashell can find
# it via QML_IMPORT_PATH (we'll add ~/.local/lib/qt6/qml to plasma's env).
install(DIRECTORY ${CMAKE_BINARY_DIR}/qml/local/lwepaper/
        DESTINATION lib/qt6/qml/local/lwepaper
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
# Also install the underlying QML-module library next to the plugin shim so
# RPATH=$ORIGIN finds it. Without this the plugin loads but cannot resolve
# its own symbols at runtime.
install(TARGETS lwepaperplugin LIBRARY DESTINATION lib/qt6/qml/local/lwepaper)
# Make the plugin shim look for its sibling lib in its own dir.
set_target_properties(lwepaperpluginplugin PROPERTIES
    INSTALL_RPATH "$ORIGIN"
    BUILD_WITH_INSTALL_RPATH TRUE
)

# Install the Plasma wallpaper plugin package alongside.
install(DIRECTORY wallpaper/
        DESTINATION share/plasma/wallpapers/local.lwepaper
)
