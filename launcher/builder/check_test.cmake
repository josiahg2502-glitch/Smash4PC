# ttt2-build's check of this computer in situations made for it (cmake -P,
# from CMakeLists.txt's tests), in WORK (deleted first):
#
# masquerade  ccache's folder first in PATH, where clang++ and g++ are links
#             to one program and clang is not installed: g++ is still tried
# broken_kit  a copy of the kit whose runtime needs a C++ library function
#             this system lacks: the check step says so, before any build
#
# BUILD the kit's ttt2-build, CXX this build's compiler, AR its archiver.

file(REMOVE_RECURSE ${WORK})
file(MAKE_DIRECTORY ${WORK})

function(run_check expected)
    execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE out ERROR_VARIABLE log RESULT_VARIABLE result)
    message("${log}${out}")
    foreach(text IN LISTS expected)
        string(FIND "${out}${log}" "${text}" at)
        if(at EQUAL -1)
            message(FATAL_ERROR "${CASE}: no \"${text}\" in what ttt2-build said (exit ${result})")
        endif()
    endforeach()
endfunction()

if(CASE STREQUAL "masquerade")
    set(masq ${WORK}/masq)
    file(MAKE_DIRECTORY ${masq})
    file(CONFIGURE OUTPUT ${WORK}/ccache CONTENT [[
#!/bin/sh
# As ccache's links run the compiler they are named after.
case "${0##*/}" in
g++) exec "@CXX@" "$@" ;;
esac
echo "ccache: error: Could not find compiler \"${0##*/}\" in PATH" >&2
exit 1
]] @ONLY)
    file(CHMOD ${WORK}/ccache PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    file(CREATE_LINK ../ccache ${masq}/clang++ SYMBOLIC)
    file(CREATE_LINK ../ccache ${masq}/g++ SYMBOLIC)
    set(expected "@ok")
    # (Versioned compilers elsewhere in PATH would be tried before g++.)
    string(REPLACE ":" ";" folders "$ENV{PATH}")
    set(versioned)
    foreach(folder IN LISTS folders)
        file(GLOB found "${folder}/clang++-[0-9]*" "${folder}/g++-[0-9]*")
        list(APPEND versioned ${found})
    endforeach()
    if(NOT versioned)
        list(APPEND expected "(${masq}/g++)")
    endif()
    run_check("${expected}" ${CMAKE_COMMAND} -E env --unset=CXX "PATH=${masq}:$ENV{PATH}"
        ${BUILD} check --work ${WORK}/work)
elseif(CASE STREQUAL "broken_kit")
    get_filename_component(kit ${BUILD} DIRECTORY)
    get_filename_component(kit ${kit} DIRECTORY)
    file(COPY ${kit}/ DESTINATION ${WORK}/kit)
    file(WRITE ${WORK}/newer.cpp [[
// As if the runtime had been compiled against a newer C++ library.
namespace std {
void ttt2_kit_test_newer();
}
void ttt2_kit_test() { std::ttt2_kit_test_newer(); }
]])
    execute_process(COMMAND ${CXX} -fPIE -c ${WORK}/newer.cpp -o ${WORK}/newer.o RESULT_VARIABLE failed)
    if(NOT failed)
        execute_process(COMMAND ${AR} q ${WORK}/kit/lib/libttt2_runtime.a ${WORK}/newer.o RESULT_VARIABLE failed)
    endif()
    if(failed)
        message(FATAL_ERROR "${CASE}: cannot make the broken kit")
    endif()
    run_check("@error The game program cannot be linked on this system;older than the launcher's;ttt2_kit_test_newer"
        ${CMAKE_COMMAND} -E env --unset=CXX ${WORK}/kit/bin/ttt2-build check --compiler ${CXX} --work ${WORK}/work)
else()
    message(FATAL_ERROR "unknown case ${CASE}")
endif()
file(REMOVE_RECURSE ${WORK})
