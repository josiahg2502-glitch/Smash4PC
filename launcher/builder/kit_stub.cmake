# Writes the generated code's stub (cmake -P, from CMakeLists.txt): what the
# kit's archives use of the code cafe-recomp generates, defined empty, so
# that ttt2-build's check step can link the kit as the game program will be
# linked, before the build. That is the tables of cafe/generated.h (with no
# entries) and the guest functions the runtime calls or hooks, by the names
# cafe-recomp gives them (sub_XXXXXXXX, sub_XXXXXXXX_orig): only names the
# runtime's own sources use, nothing of the game.
#
# NM        nm or llvm-nm
# ARCHIVES  the kit's archives
# OUTPUT    the C++ file to write

set(used)
set(defined)
foreach(archive IN LISTS ARCHIVES)
    # POSIX format, the same from both: "name type [value size]".
    execute_process(COMMAND ${NM} -g -P ${archive} OUTPUT_VARIABLE symbols RESULT_VARIABLE failed)
    if(failed)
        message(FATAL_ERROR "generated_stub.cpp: ${NM} cannot read ${archive}")
    endif()
    string(REGEX MATCHALL "(^|\n)[^ \n]+ [A-Za-z]" entries "${symbols}")
    foreach(entry IN LISTS entries)
        string(STRIP "${entry}" entry)
        string(REGEX REPLACE " .*" "" name "${entry}")
        # (w and v: weak references, which need no definition.)
        if(entry MATCHES " U$")
            list(APPEND used ${name})
        elseif(NOT entry MATCHES " [wv]$")
            list(APPEND defined ${name})
        endif()
    endforeach()
endforeach()
set(generated "^sub_[0-9A-F]+(_orig)?$")
list(FILTER used INCLUDE REGEX "${generated}")
list(FILTER defined INCLUDE REGEX "${generated}")
list(REMOVE_DUPLICATES used)
if(defined)
    list(REMOVE_ITEM used ${defined})
endif()
list(SORT used)

set(functions "")
foreach(name IN LISTS used)
    string(APPEND functions "PPC_FUNC(${name}) {}\n")
endforeach()
file(CONFIGURE OUTPUT ${OUTPUT} CONTENT [[
// Made by launcher/builder/kit_stub.cmake: what the kit's runtime uses of
// the generated code, empty, for ttt2-build's check that the kit links.
#include "cafe/generated.h"
#include "cafe/ppc_ops.h"

extern "C" const cafe::GuestFunction cafe_guest_functions[1] = {};
extern "C" const size_t cafe_guest_function_count = 0;
extern "C" const cafe::GuestImport cafe_guest_imports[1] = {};
extern "C" const size_t cafe_guest_import_count = 0;
extern "C" const cafe::ProgramInfo cafe_program_info = {};

@functions@]] @ONLY)
