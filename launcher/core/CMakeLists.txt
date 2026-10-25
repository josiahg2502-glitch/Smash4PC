# The launcher's logic without GTK: finding the install, the kit, the importer
# and Python; reading the builder's and the importer's output; checking the
# PC before an install; uninstalling; the game's keyboard keys and display
# settings; the launcher's own updates. Tested on its own.
#
# display.ini is read and written by the game's own code for it (runtime/src,
# found from here so that this folder also builds as a project's
# subdirectory of its own): what the launcher writes is what the game reads.
set(runtime_src ${CMAKE_CURRENT_SOURCE_DIR}/../../runtime/src)
add_library(ttt2_launcher_core STATIC
    display.cpp
    files.cpp
    json.cpp
    keymap.cpp
    paths.cpp
    protocol.cpp
    release.cpp
    status.cpp
    system.cpp
    test_actions.cpp
    uninstall.cpp
    ${runtime_src}/host/display_settings.cpp)
target_include_directories(ttt2_launcher_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${runtime_src})
target_compile_features(ttt2_launcher_core PUBLIC cxx_std_20)
target_compile_options(ttt2_launcher_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(launcher_core_test core_test.cpp)
target_link_libraries(launcher_core_test PRIVATE ttt2_launcher_core)
target_compile_options(launcher_core_test PRIVATE -Wall -Wextra)
add_test(NAME launcher_core_test COMMAND launcher_core_test ${CMAKE_CURRENT_BINARY_DIR})

# The key tables are cross-checked against Linux's and SDL3's headers when
# this computer has them (where they are looked for when the test runs).
find_file(TTT2_INPUT_EVENT_CODES_H linux/input-event-codes.h)
find_file(TTT2_SDL_SCANCODE_H SDL3/SDL_scancode.h)
foreach(header TTT2_INPUT_EVENT_CODES_H TTT2_SDL_SCANCODE_H)
    if(NOT ${header})
        set(${header} /nonexistent/${header})
    endif()
endforeach()
add_executable(launcher_keymap_test keymap_test.cpp)
target_link_libraries(launcher_keymap_test PRIVATE ttt2_launcher_core)
target_compile_options(launcher_keymap_test PRIVATE -Wall -Wextra)
add_test(NAME launcher_keymap_test
    COMMAND launcher_keymap_test ${CMAKE_CURRENT_BINARY_DIR} ${TTT2_INPUT_EVENT_CODES_H} ${TTT2_SDL_SCANCODE_H})
