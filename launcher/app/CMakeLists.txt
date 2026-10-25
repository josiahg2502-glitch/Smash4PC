# ttt2-launcher: the players' window over the builder and the importer, in
# plain GTK 4 (no libadwaita, no C++ bindings: few libraries to ship in the
# AppImage). Without GTK 4.10 or newer it is left out.

# The importer, as the launcher runs it installed: read-only, without its
# tests and caches.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/characters/ DESTINATION share/ttt2/characters
    PATTERN tests EXCLUDE
    PATTERN __pycache__ EXCLUDE
    PATTERN .pytest_cache EXCLUDE
    PATTERN "*.pyc" EXCLUDE)

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GTK4 QUIET IMPORTED_TARGET gtk4>=4.10)
endif()
if(NOT GTK4_FOUND)
    message(STATUS "GTK 4.10 or newer not found: ttt2-launcher is left out")
    return()
endif()

set(TTT2_LAUNCHER_APP_ID io.github.petexy.TekkenTag2UltimateEdition)
# (Its version is the kit's, set in builder/: a release is tagged v<version>.)
if(NOT TTT2_LAUNCHER_VERSION)
    message(FATAL_ERROR "TTT2_LAUNCHER_VERSION is not set (launcher/builder sets it)")
endif()
if(NOT TTT2_KIT_DIR)
    set(TTT2_KIT_DIR ${CMAKE_BINARY_DIR}/launcher/kit)
endif()

# The launcher's own icons (Settings, Back, Update, the GamePad's inputs),
# built into the program as a GResource: they are there whether it runs from
# the build tree, an install or the AppImage. Its icon theme finds them under
# <prefix>/icons/scalable/actions.
set(TTT2_LAUNCHER_ICON_DIR ${CMAKE_CURRENT_SOURCE_DIR}/icons CACHE PATH "The folder of the launcher's SVG icons")
set(launcher_icons ttt2-settings-symbolic ttt2-back-symbolic ttt2-update-symbolic)
foreach(input dpad_up dpad_down dpad_left dpad_right face_top face_bottom face_left face_right l r zl zr
              stick_up stick_down stick_left stick_right plus minus home)
    list(APPEND launcher_icons ttt2-input-${input})
endforeach()
pkg_get_variable(glib_compile_resources gio-2.0 glib_compile_resources)
find_program(TTT2_GLIB_COMPILE_RESOURCES NAMES ${glib_compile_resources} glib-compile-resources)
set(icon_files "")
set(icon_list "")
foreach(icon ${launcher_icons})
    if(NOT EXISTS ${TTT2_LAUNCHER_ICON_DIR}/${icon}.svg)
        list(APPEND missing_icons ${icon}.svg)
    endif()
    list(APPEND icon_files ${TTT2_LAUNCHER_ICON_DIR}/${icon}.svg)
    string(APPEND icon_list "    <file>${icon}.svg</file>\n")
endforeach()
if(NOT TTT2_GLIB_COMPILE_RESOURCES)
    message(WARNING "glib-compile-resources (GLib's development files) not found: ttt2-launcher is left out")
    return()
endif()
if(missing_icons)
    message(WARNING "${TTT2_LAUNCHER_ICON_DIR} lacks ${missing_icons}: ttt2-launcher is left out")
    return()
endif()
set(TTT2_LAUNCHER_RESOURCE_PATH /io/github/petexy/TekkenTag2UltimateEdition)
set(icons_xml ${CMAKE_CURRENT_BINARY_DIR}/icons.gresource.xml)
file(CONFIGURE OUTPUT ${icons_xml} CONTENT
     "<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<gresources>
  <gresource prefix=\"${TTT2_LAUNCHER_RESOURCE_PATH}/icons/scalable/actions\">
${icon_list}  </gresource>
</gresources>
")
add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icons.c
    COMMAND ${TTT2_GLIB_COMPILE_RESOURCES} --sourcedir=${TTT2_LAUNCHER_ICON_DIR} --generate-source
            --c-name=ttt2_launcher_icons --target=${CMAKE_CURRENT_BINARY_DIR}/icons.c ${icons_xml}
    DEPENDS ${icons_xml} ${icon_files}
    COMMENT "Building the launcher's icons into it"
    VERBATIM)

add_executable(ttt2-launcher
    main.cpp
    characters.cpp
    game.cpp
    launcher.cpp
    process.cpp
    self_update.cpp
    settings.cpp
    task.cpp
    widgets.cpp
    ${CMAKE_CURRENT_BINARY_DIR}/icons.c)
target_link_libraries(ttt2-launcher PRIVATE ttt2_launcher_core PkgConfig::GTK4)
# (Not on the generated icons.c.)
target_compile_options(ttt2-launcher PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic>)
# The build tree's kit and the source tree's importer and icon are found
# when the launcher runs from the build tree (an installed one has its own).
# A package turns that off (launcher/appimage/make_appimage.sh): then the
# program names no folder of the computer it was built on.
option(TTT2_LAUNCHER_BUILD_TREE_PATHS "ttt2-launcher also looks in its build and source trees" ON)
if(TTT2_LAUNCHER_BUILD_TREE_PATHS)
    set(build_kit ${TTT2_KIT_DIR})
    set(source_importer ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/characters)
    set(source_icon ${CMAKE_CURRENT_SOURCE_DIR})
else()
    set(build_kit "")
    set(source_importer "")
    set(source_icon "")
endif()
# Only what GTK 4.10 and the GLib it needs (2.76) have, so that it runs on the
# distributions that ship them (newer GLib headers' type macros otherwise
# call 2.80's g_once_init_enter_pointer).
target_compile_definitions(ttt2-launcher PRIVATE
    GDK_VERSION_MIN_REQUIRED=GDK_VERSION_4_10
    GDK_VERSION_MAX_ALLOWED=GDK_VERSION_4_10
    GLIB_VERSION_MIN_REQUIRED=GLIB_VERSION_2_76
    GLIB_VERSION_MAX_ALLOWED=GLIB_VERSION_2_76
    TTT2_LAUNCHER_APP_ID="${TTT2_LAUNCHER_APP_ID}"
    TTT2_LAUNCHER_VERSION="${TTT2_LAUNCHER_VERSION}"
    TTT2_LAUNCHER_RESOURCE_PATH="${TTT2_LAUNCHER_RESOURCE_PATH}"
    TTT2_LAUNCHER_BUILD_KIT="${build_kit}"
    TTT2_LAUNCHER_SOURCE_IMPORTER="${source_importer}"
    TTT2_LAUNCHER_SOURCE_ICON="${source_icon}")
set_target_properties(ttt2-launcher PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/launcher)

install(TARGETS ttt2-launcher RUNTIME DESTINATION bin)
install(FILES ${TTT2_LAUNCHER_APP_ID}.desktop DESTINATION share/applications)
install(FILES ${TTT2_LAUNCHER_APP_ID}.svg DESTINATION share/icons/hicolor/scalable/apps)
