# Differential semantics test: generated code vs Dolphin's interpreter.
# Needs the Dolphin reference checkout (third_party/ref/dolphin) and the game
# executable (TTT2_RPX), which supplies the instruction encodings under test.
set(TTT2_RPX "${CMAKE_SOURCE_DIR}/local/wua/000500001010f800_v16/code/Tekken.rpx"
    CACHE FILEPATH "Tekken.rpx used to sample test encodings")
set(DOLPHIN_CORE "${CMAKE_SOURCE_DIR}/third_party/ref/dolphin/Source/Core")
find_package(fmt QUIET)

if(EXISTS "${DOLPHIN_CORE}/Core/PowerPC/Interpreter/Interpreter_Tables.cpp"
   AND EXISTS "${TTT2_RPX}" AND fmt_FOUND)
    # Dolphin's Interpreter.h minus the emulator core interface, derived at
    # configure time so no Dolphin source is copied into this tree.
    file(READ "${DOLPHIN_CORE}/Core/PowerPC/Interpreter/Interpreter.h" interpreter_h)
    string(REPLACE " : public CPUCoreBase" "" interpreter_h "${interpreter_h}")
    string(REPLACE " override;" ";" interpreter_h "${interpreter_h}")
    string(REPLACE "private:" "public:" interpreter_h "${interpreter_h}")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/oracle_shim/Core/PowerPC/Interpreter/Interpreter.h"
        "${interpreter_h}")
    # Likewise PowerPC.h's register state and access macros, without the
    # core manager and the debugger (oracle/PowerPC.h.in).
    file(READ "${DOLPHIN_CORE}/Core/PowerPC/PowerPC.h" powerpc_h)
    string(FIND "${powerpc_h}" "// TLB cache" state_begin)
    string(FIND "${powerpc_h}" "#ifdef _M_X86_64\n#ifdef __GNUC__" state_end)
    string(FIND "${powerpc_h}" "// Easy register access macros." macros_begin)
    string(FIND "${powerpc_h}" "void RoundingModeUpdated" macros_end)
    foreach(position state_begin state_end macros_begin macros_end)
        if(${position} LESS 0)
            message(FATAL_ERROR "semantics_test: Dolphin's PowerPC.h has changed (${position} not found)")
        endif()
    endforeach()
    math(EXPR state_length "${state_end} - ${state_begin}")
    math(EXPR macros_length "${macros_end} - ${macros_begin}")
    string(SUBSTRING "${powerpc_h}" ${state_begin} ${state_length} powerpc_state)
    string(SUBSTRING "${powerpc_h}" ${macros_begin} ${macros_length} powerpc_macros)
    string(REPLACE "InstructionCache iCache;" "NoopCache iCache;" powerpc_state "${powerpc_state}")
    string(REPLACE "Cache dCache;" "NoopCache dCache;" powerpc_state "${powerpc_state}")
    configure_file(oracle/PowerPC.h.in "${CMAKE_CURRENT_BINARY_DIR}/oracle_shim/Core/PowerPC/PowerPC.h" @ONLY)

    set(interp "${DOLPHIN_CORE}/Core/PowerPC/Interpreter")
    add_library(dolphin_oracle STATIC
        oracle/oracle.cpp
        ${interp}/Interpreter_Tables.cpp
        ${interp}/Interpreter_Integer.cpp
        ${interp}/Interpreter_LoadStore.cpp
        ${interp}/Interpreter_FloatingPoint.cpp
        ${interp}/Interpreter_Paired.cpp
        ${interp}/Interpreter_LoadStorePaired.cpp
        ${interp}/Interpreter_SystemRegisters.cpp
        ${interp}/Interpreter_Branch.cpp
        ${DOLPHIN_CORE}/Core/PowerPC/ConditionRegister.cpp
        ${DOLPHIN_CORE}/Common/FloatUtils.cpp)
    target_include_directories(dolphin_oracle PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/oracle_shim" oracle/shim "${DOLPHIN_CORE}")
    target_compile_features(dolphin_oracle PRIVATE cxx_std_23)
    target_compile_options(dolphin_oracle PRIVATE -w)
    target_link_libraries(dolphin_oracle PRIVATE fmt::fmt)

    set(semantics_cases "${CMAKE_CURRENT_BINARY_DIR}/semantics_cases.cpp")
    add_custom_command(OUTPUT "${semantics_cases}"
        COMMAND cafe-semgen "${TTT2_RPX}" "${semantics_cases}" 48
        DEPENDS cafe-semgen
        COMMENT "Sampling instruction encodings from Tekken.rpx"
        VERBATIM)
    add_executable(semantics_test oracle/semantics_test.cpp "${semantics_cases}")
    target_include_directories(semantics_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
    target_link_libraries(semantics_test PRIVATE dolphin_oracle cafe_recomp)
    target_compile_features(semantics_test PRIVATE cxx_std_20)
    target_compile_options(semantics_test PRIVATE -O2 -march=x86-64-v3 -Wno-unused-label)
    add_test(NAME semantics_test COMMAND semantics_test)

    add_executable(cafe-cfgen oracle/cfgen_main.cpp)
    target_link_libraries(cafe-cfgen PRIVATE cafe_recomp)
    set(control_flow_cases "${CMAKE_CURRENT_BINARY_DIR}/control_flow_cases.cpp")
    add_custom_command(OUTPUT "${control_flow_cases}"
        COMMAND cafe-cfgen "${control_flow_cases}"
        DEPENDS cafe-cfgen
        COMMENT "Assembling control-flow test programs"
        VERBATIM)
    add_executable(control_flow_test oracle/control_flow_test.cpp "${control_flow_cases}")
    target_include_directories(control_flow_test PRIVATE
        ${CMAKE_SOURCE_DIR}/runtime/include ${CMAKE_CURRENT_SOURCE_DIR}/oracle)
    target_link_libraries(control_flow_test PRIVATE dolphin_oracle)
    target_compile_features(control_flow_test PRIVATE cxx_std_20)
    target_compile_options(control_flow_test PRIVATE -O2 -march=x86-64-v3 -Wno-unused-label)
    add_test(NAME control_flow_test COMMAND control_flow_test)
else()
    message(STATUS "semantics_test disabled: needs third_party/ref/dolphin, fmt and TTT2_RPX")
endif()

# The game's files from a .wua whose update is merged or beside the base
# game (synthetic archives; no game data needed).
add_executable(wua_layers_test wua_layers_test.cpp ${CMAKE_SOURCE_DIR}/runtime/src/wua.cpp)
target_include_directories(wua_layers_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src)
target_compile_features(wua_layers_test PRIVATE cxx_std_20)
target_link_libraries(wua_layers_test PRIVATE ZArchive::zarchive)
add_test(NAME wua_layers_test COMMAND wua_layers_test)

# The title's file formats, icons and texts (synthetic data; no game files).
add_executable(title_test title_test.cpp)
target_link_libraries(title_test PRIVATE cafe_title)
target_compile_options(title_test PRIVATE -Wall -Wextra)
add_test(NAME title_test COMMAND title_test)

# The keyboard's keys from keyboard.ini and its reloading (files in a
# scratch folder of the build tree; SDL only for its key names).
find_package(SDL3 REQUIRED CONFIG)
add_executable(keymap_test keymap_test.cpp ${CMAKE_SOURCE_DIR}/runtime/src/host/keymap.cpp)
target_include_directories(keymap_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src ${CMAKE_SOURCE_DIR}/runtime/include)
target_compile_features(keymap_test PRIVATE cxx_std_20)
target_link_libraries(keymap_test PRIVATE SDL3::SDL3)
target_compile_options(keymap_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME keymap_test COMMAND keymap_test ${CMAKE_CURRENT_BINARY_DIR}/keymap_test.d)

# keyboard.ini as the launcher writes it, read by the game's parser: their
# inputs, defaults and reserved keys stay the same.
add_executable(keyboard_file_test keyboard_file_test.cpp ${CMAKE_SOURCE_DIR}/runtime/src/host/keymap.cpp)
target_include_directories(keyboard_file_test PRIVATE
    ${CMAKE_SOURCE_DIR}/runtime/src ${CMAKE_SOURCE_DIR}/runtime/include)
target_link_libraries(keyboard_file_test PRIVATE ttt2_launcher_core SDL3::SDL3)
target_compile_options(keyboard_file_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME keyboard_file_test COMMAND keyboard_file_test)

# The display settings from display.ini, the environment over them and the
# window's size (files in a scratch folder of the build tree; no SDL).
add_executable(display_settings_test display_settings_test.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/host/display_settings.cpp)
target_include_directories(display_settings_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src)
target_compile_features(display_settings_test PRIVATE cxx_std_20)
target_compile_options(display_settings_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME display_settings_test
    COMMAND display_settings_test ${CMAKE_CURRENT_BINARY_DIR}/display_settings_test.d)

# Guest polling loops must see other threads' stores (no Dolphin needed).
add_executable(memory_model_test memory_model_test.cpp)
target_include_directories(memory_model_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime/include)
target_compile_features(memory_model_test PRIVATE cxx_std_20)
# The flags the port compiles generated code with.
target_compile_options(memory_model_test PRIVATE -O2 -march=x86-64-v3 -Wno-unused-label)
add_test(NAME memory_model_test COMMAND memory_model_test)

# The character importer's formats (tools/characters; synthetic data, no game
# files). Needs Python 3 with numpy and Pillow.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
    execute_process(COMMAND "${Python3_EXECUTABLE}" -c "import numpy, PIL"
        RESULT_VARIABLE character_import_modules OUTPUT_QUIET ERROR_QUIET)
    if(character_import_modules EQUAL 0)
        add_test(NAME character_import_test
            COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s "${CMAKE_SOURCE_DIR}/tools/characters/tests")
    else()
        message(STATUS "character_import_test disabled: needs Python 3 with numpy and Pillow")
    endif()
endif()
