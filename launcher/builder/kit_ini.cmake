# Writes the kit's kit.ini (cmake -P, from CMakeLists.txt): what ttt2-build
# passes to the player's compiler. ${KIT} stands for the kit's folder.
#
# compile    for each generated file, as the ttt2 target compiles them
# link       after the generated objects: the runtime archive as a whole,
#            then the other archives
# libraries  the system's shared libraries by soname, which ttt2-build finds
#            on the player's PC (they need not have the development files)
# flags      link flags last
# needs      what the kit's programs and the game program need of the C and
#            C++ libraries, in words, for ttt2-build's messages
#
# It is written once the kit is known to work here: the generated code's
# stub (STUB_SOURCE) compiled and linked with these lines by this build's
# compiler (CXX) into a program (CHECK) that starts. The stub's object
# (STUB_OBJECT) is the kit's, for the same check on the player's PC.

set(compile_arguments -std=gnu++20 ${COMPILE} -fPIE -I\${KIT}/include)
list(JOIN compile_arguments " " compile)

list(POP_FRONT ARCHIVES runtime)
set(link_arguments -pie -Wl,--whole-archive \${KIT}/lib/${runtime} -Wl,--no-whole-archive -Wl,--start-group)
foreach(archive IN LISTS ARCHIVES)
    list(APPEND link_arguments \${KIT}/lib/${archive})
endforeach()
list(APPEND link_arguments -Wl,--end-group)
list(JOIN link_arguments " " link)

file(STRINGS ${LIBRARIES} files)
set(sonames)
foreach(file IN LISTS files)
    execute_process(COMMAND ${READELF} -d ${file} OUTPUT_VARIABLE dynamic RESULT_VARIABLE failed ERROR_QUIET)
    if(failed OR NOT dynamic MATCHES "\\(SONAME\\)[^\n]*\\[([^]\n]+)\\]")
        message(FATAL_ERROR "kit.ini: ${file} has no soname")
    endif()
    list(APPEND sonames ${CMAKE_MATCH_1})
endforeach()
list(REMOVE_DUPLICATES sonames)
list(JOIN sonames " " libraries)

# Threads for any glibc: before 2.34 they were a library of their own.
list(APPEND FLAGS -pthread)
list(REMOVE_DUPLICATES FLAGS)
list(JOIN FLAGS " " flags)

# The stub, as ttt2-build compiles and links (the libraries by their files).
get_filename_component(kit ${KIT_INI} DIRECTORY)
string(REPLACE "\${KIT}" "${kit}" compile_arguments "${compile_arguments}")
string(REPLACE "\${KIT}" "${kit}" link_arguments "${link_arguments}")
execute_process(COMMAND ${CXX} ${compile_arguments} -c ${STUB_SOURCE} -o ${STUB_OBJECT} RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "kit.ini: the generated code's stub does not compile")
endif()
execute_process(COMMAND ${CXX} -o ${CHECK} ${STUB_OBJECT} ${link_arguments} ${files} ${FLAGS} RESULT_VARIABLE failed)
if(failed)
    message(FATAL_ERROR "kit.ini: the kit does not link with the generated code's stub (kit_stub.cmake)")
endif()
execute_process(COMMAND ${CHECK} --help RESULT_VARIABLE failed OUTPUT_QUIET ERROR_QUIET)
if(failed)
    message(FATAL_ERROR "kit.ini: the program linked with the generated code's stub does not start (${CHECK} --help)")
endif()

# The newest versions of glibc's and libstdc++'s symbols that cafe-recomp
# and the game program need.
set(glibc 0)
set(glibcxx 0)
foreach(program ${CHECK} ${RECOMP})
    execute_process(COMMAND ${READELF} -V ${program} OUTPUT_VARIABLE versions RESULT_VARIABLE failed ERROR_QUIET)
    if(failed)
        message(FATAL_ERROR "kit.ini: ${READELF} cannot read ${program}")
    endif()
    string(REGEX MATCHALL "Name: GLIBC(XX)?_[0-9.]+" names "${versions}")
    foreach(name IN LISTS names)
        string(REGEX MATCH "(GLIBCX*)_(.+)" name "${name}")
        set(library ${CMAKE_MATCH_1})
        set(number ${CMAKE_MATCH_2})
        if(library STREQUAL "GLIBC" AND number VERSION_GREATER "${glibc}")
            set(glibc ${number})
        elseif(library STREQUAL "GLIBCXX" AND number VERSION_GREATER "${glibcxx}")
            set(glibcxx ${number})
        endif()
    endforeach()
endforeach()
# The GCC release whose libstdc++ first has each version (libstdc++'s ABI
# documentation); a later one is named by its version alone.
set(gcc_releases 3.4.29 11 3.4.30 12 3.4.31 13 3.4.32 13 3.4.33 14 3.4.34 15 3.4.35 16)
set(needs "glibc ${glibc}")
if(NOT glibcxx VERSION_EQUAL 0)
    list(FIND gcc_releases ${glibcxx} at)
    if(at GREATER_EQUAL 0)
        math(EXPR at "${at} + 1")
        list(GET gcc_releases ${at} gcc)
        string(APPEND needs " and GCC ${gcc}'s libstdc++ (GLIBCXX_${glibcxx})")
    else()
        string(APPEND needs " and libstdc++ with GLIBCXX_${glibcxx}")
    endif()
endif()

file(CONFIGURE OUTPUT ${KIT_INI} CONTENT [[
# How ttt2-build compiles and links the game program (made by
# launcher/builder/kit_ini.cmake). ${KIT} is this folder.
version = @VERSION@
compile = @compile@
link = @link@
libraries = @libraries@
flags = @flags@
needs = @needs@
]] @ONLY)
