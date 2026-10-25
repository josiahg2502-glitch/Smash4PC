# The kit the launcher builds the game program with on the player's PC: the
# player's .wua goes through cafe-recomp, the system's C++ compiler compiles
# the generated code, and it is linked with the runtime as this build made it
# for the ttt2 target. Nothing of the game is in the kit:
#
# kit/bin/ttt2-build     runs the steps (src/; see ttt2-build --help)
# kit/bin/cafe-recomp
# kit/include/cafe/      the headers the generated code includes
# kit/lib/*.a            the runtime and main.cpp (linked whole: the runtime's
#                        imports must replace the generated weak defaults and
#                        its export registrations must stay), and the archives
#                        it uses
# kit/lib/generated_stub.o  what the runtime uses of the generated code,
#                        empty (kit_stub.cmake): ttt2-build links it with the
#                        kit to know the program will link and start
# kit/kit.ini            how to compile and link (kit_ini.cmake)
# kit/VERSION            the launcher's version and this build's commit
# kit/t7helper.exe       the Tekken 7 helper, when MinGW is there to build it
#
# The same layout is installed to <prefix>/lib/ttt2/kit.

set(TTT2_LAUNCHER_VERSION 0.1.1)
set(TTT2_LAUNCHER_VERSION ${TTT2_LAUNCHER_VERSION} PARENT_SCOPE)
set(TTT2_KIT_DIR ${CMAKE_BINARY_DIR}/launcher/kit)
set(TTT2_KIT_DIR ${TTT2_KIT_DIR} PARENT_SCOPE)

# The player's compiler links the kit's objects into a position-independent
# executable, so they must be position-independent too. Every current
# distribution's compilers make them so by default; the kit reuses the
# objects ttt2 is linked from rather than compiling the runtime twice.
include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#if !defined(__PIE__)\n#error\n#endif\nint main() {}" TTT2_KIT_PIE)
if(NOT TTT2_KIT_PIE)
    message(WARNING "the launcher's kit is left out: ${CMAKE_CXX_COMPILER} does not make position-independent "
                    "code by default (configure with -DCMAKE_C_FLAGS=-fPIE -DCMAKE_CXX_FLAGS=-fPIE)")
    return()
endif()
if(CMAKE_CXX_FLAGS MATCHES "-flto" OR CMAKE_INTERPROCEDURAL_OPTIMIZATION)
    message(WARNING "the launcher's kit holds link-time-optimisation objects, which only the same compiler can link")
endif()

# The version an install was built with is in its .ttt2-build: the launcher
# offers Update when the kit's differs. The commit is read again after each
# commit (CMake re-runs when HEAD or the branch moves).
find_package(Git QUIET)
set(TTT2_KIT_VERSION ${TTT2_LAUNCHER_VERSION})
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=12 HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE commit OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE failed ERROR_QUIET)
    if(NOT failed AND commit)
        string(APPEND TTT2_KIT_VERSION "-${commit}")
        execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE branch OUTPUT_STRIP_TRAILING_WHITESPACE)
        foreach(file HEAD packed-refs ${branch})
            if(EXISTS ${git_dir}/${file})
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${git_dir}/${file})
            endif()
        endforeach()
    endif()
endif()
file(CONFIGURE OUTPUT ${TTT2_KIT_DIR}/VERSION CONTENT "${TTT2_KIT_VERSION}\n")

# What linking cafe_runtime brings into ttt2, as CMake resolves it: the
# archives built here (copied into the kit) and the system's libraries
# (named in kit.ini by their sonames, found on the player's PC). The
# packages' targets are visible only where they were found, so they are
# found here too (with the same, cached, result).
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(ZLIB REQUIRED)
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
function(ttt2_link_closure item)
    string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" item "${item}")
    if(TARGET ${item})
        get_target_property(aliased ${item} ALIASED_TARGET)
        if(aliased)
            set(item ${aliased})
        endif()
        get_property(seen GLOBAL PROPERTY TTT2_KIT_SEEN)
        if(item IN_LIST seen)
            return()
        endif()
        set_property(GLOBAL APPEND PROPERTY TTT2_KIT_SEEN ${item})
        get_target_property(type ${item} TYPE)
        get_target_property(imported ${item} IMPORTED)
        if(type STREQUAL "STATIC_LIBRARY")
            set_property(GLOBAL APPEND PROPERTY TTT2_KIT_ARCHIVES ${item})
        elseif(imported AND type MATCHES "SHARED_LIBRARY|UNKNOWN_LIBRARY")
            set_property(GLOBAL APPEND PROPERTY TTT2_KIT_LIBRARIES "$<TARGET_FILE:${item}>")
        elseif(NOT type MATCHES "INTERFACE_LIBRARY|OBJECT_LIBRARY")
            message(FATAL_ERROR "the launcher's kit cannot link ${item} (${type})")
        endif()
        get_target_property(dependencies ${item} INTERFACE_LINK_LIBRARIES)
        foreach(dependency IN LISTS dependencies)
            if(dependency)
                ttt2_link_closure(${dependency})
            endif()
        endforeach()
    elseif(item MATCHES "\\$<|::")
        message(FATAL_ERROR "the launcher's kit cannot link ${item}")
    elseif(IS_ABSOLUTE ${item} AND item MATCHES "\\.a$")
        message(FATAL_ERROR "the launcher's kit cannot link the system's archive ${item}")
    elseif(IS_ABSOLUTE ${item})
        set_property(GLOBAL APPEND PROPERTY TTT2_KIT_LIBRARIES ${item})
    else()
        set_property(GLOBAL APPEND PROPERTY TTT2_KIT_FLAGS ${item})
    endif()
endfunction()
ttt2_link_closure(cafe_runtime)
get_property(archive_targets GLOBAL PROPERTY TTT2_KIT_ARCHIVES)
get_property(libraries GLOBAL PROPERTY TTT2_KIT_LIBRARIES)
get_property(link_flags GLOBAL PROPERTY TTT2_KIT_FLAGS)

# main.cpp as the ttt2 target compiles it, and the runtime's objects, in the
# archive the player's program is linked from as a whole.
add_library(ttt2_kit_main OBJECT ${CMAKE_SOURCE_DIR}/runtime/src/main.cpp)
target_link_libraries(ttt2_kit_main PRIVATE cafe_runtime)
target_compile_options(ttt2_kit_main PRIVATE -Wall -Wextra -Wpedantic)
add_library(ttt2_kit_runtime STATIC $<TARGET_OBJECTS:cafe_runtime> $<TARGET_OBJECTS:ttt2_kit_main>)
set_target_properties(ttt2_kit_runtime PROPERTIES
    OUTPUT_NAME ttt2_runtime ARCHIVE_OUTPUT_DIRECTORY ${TTT2_KIT_DIR}/lib LINKER_LANGUAGE CXX)

set(kit_files)
set(kit_archives libttt2_runtime.a)
set(kit_archive_files ${TTT2_KIT_DIR}/lib/libttt2_runtime.a)
foreach(target IN LISTS archive_targets)
    set(name ${CMAKE_STATIC_LIBRARY_PREFIX}${target}${CMAKE_STATIC_LIBRARY_SUFFIX})
    add_custom_command(OUTPUT ${TTT2_KIT_DIR}/lib/${name}
        COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:${target}> ${TTT2_KIT_DIR}/lib/${name}
        DEPENDS ${target} VERBATIM)
    list(APPEND kit_files ${TTT2_KIT_DIR}/lib/${name})
    list(APPEND kit_archives ${name})
    list(APPEND kit_archive_files ${TTT2_KIT_DIR}/lib/${name})
endforeach()

file(GLOB headers CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/runtime/include/cafe/*.h)
set(kit_headers)
foreach(header IN LISTS headers)
    get_filename_component(name ${header} NAME)
    add_custom_command(OUTPUT ${TTT2_KIT_DIR}/include/cafe/${name}
        COMMAND ${CMAKE_COMMAND} -E copy ${header} ${TTT2_KIT_DIR}/include/cafe/${name}
        DEPENDS ${header} VERBATIM)
    list(APPEND kit_headers ${TTT2_KIT_DIR}/include/cafe/${name})
endforeach()
list(APPEND kit_files ${kit_headers})

add_custom_command(OUTPUT ${TTT2_KIT_DIR}/bin/cafe-recomp
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:cafe-recomp> ${TTT2_KIT_DIR}/bin/cafe-recomp
    DEPENDS cafe-recomp VERBATIM)
list(APPEND kit_files ${TTT2_KIT_DIR}/bin/cafe-recomp)

if(CMAKE_READELF)
    set(readelf ${CMAKE_READELF})
else()
    find_program(readelf NAMES readelf REQUIRED)
endif()
if(CMAKE_NM)
    set(nm ${CMAKE_NM})
else()
    find_program(nm NAMES nm REQUIRED)
endif()

# The generated code's stub, from what the kit's archives use of it.
set(stub_source ${CMAKE_CURRENT_BINARY_DIR}/generated_stub.cpp)
add_custom_command(OUTPUT ${stub_source}
    COMMAND ${CMAKE_COMMAND} -DNM=${nm} "-DARCHIVES=${kit_archive_files}" -DOUTPUT=${stub_source}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/kit_stub.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kit_stub.cmake ttt2_kit_runtime ${kit_archive_files} VERBATIM)

# The libraries' files as this build links them; kit_ini.cmake reads their
# sonames once they are known. It also compiles the stub into the kit, and
# links it with the kit into kit_check (not part of the kit) to prove the
# kit's lines before they are written.
file(GENERATE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/kit_libraries.txt CONTENT "$<JOIN:${libraries},\n>\n")
add_custom_command(OUTPUT ${TTT2_KIT_DIR}/kit.ini ${TTT2_KIT_DIR}/lib/generated_stub.o
    COMMAND ${CMAKE_COMMAND} -DKIT_INI=${TTT2_KIT_DIR}/kit.ini -DVERSION=${TTT2_KIT_VERSION}
        "-DCOMPILE=${TTT2_GENERATED_OPTIONS}" "-DARCHIVES=${kit_archives}" "-DFLAGS=${link_flags}"
        -DLIBRARIES=${CMAKE_CURRENT_BINARY_DIR}/kit_libraries.txt -DREADELF=${readelf}
        -DCXX=${CMAKE_CXX_COMPILER} -DSTUB_SOURCE=${stub_source} -DSTUB_OBJECT=${TTT2_KIT_DIR}/lib/generated_stub.o
        -DCHECK=${CMAKE_CURRENT_BINARY_DIR}/kit_check -DRECOMP=${TTT2_KIT_DIR}/bin/cafe-recomp
        -P ${CMAKE_CURRENT_SOURCE_DIR}/kit_ini.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/kit_ini.cmake ${CMAKE_CURRENT_BINARY_DIR}/kit_libraries.txt
        ${TTT2_KIT_DIR}/VERSION ${stub_source} ${kit_headers} ttt2_kit_runtime ${kit_archive_files}
        ${TTT2_KIT_DIR}/bin/cafe-recomp VERBATIM)
list(APPEND kit_files ${TTT2_KIT_DIR}/kit.ini ${TTT2_KIT_DIR}/lib/generated_stub.o)

# Players need no MinGW: the helper the character importer runs inside
# Tekken 7's Wine prefix is built here (as ttt2char/tekken7/autodump.py
# builds it).
find_program(TTT2_MINGW_GCC x86_64-w64-mingw32-gcc)
if(TTT2_MINGW_GCC)
    set(helper ${CMAKE_SOURCE_DIR}/tools/characters/t7helper/t7helper.c)
    add_custom_command(OUTPUT ${TTT2_KIT_DIR}/t7helper.exe
        COMMAND ${TTT2_MINGW_GCC} -O2 -mwindows -o ${TTT2_KIT_DIR}/t7helper.exe ${helper} -lws2_32
        DEPENDS ${helper} VERBATIM)
    list(APPEND kit_files ${TTT2_KIT_DIR}/t7helper.exe)
else()
    message(STATUS "No x86_64-w64-mingw32-gcc: the launcher's kit has no t7helper.exe (the importer builds it)")
endif()

# A small program that may start other programs: ZArchive (and zstd) read
# the .wua, OpenSSL checks the executable's hash.
add_executable(ttt2-build
    src/main.cpp
    src/host.cpp
    src/children.cpp
    ${CMAKE_SOURCE_DIR}/runtime/src/wua.cpp)
target_include_directories(ttt2-build PRIVATE ${CMAKE_SOURCE_DIR}/runtime/src)
target_compile_features(ttt2-build PRIVATE cxx_std_20)
target_link_libraries(ttt2-build PRIVATE ZArchive::zarchive OpenSSL::Crypto)
target_compile_options(ttt2-build PRIVATE -Wall -Wextra -Wpedantic)
set_target_properties(ttt2-build PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${TTT2_KIT_DIR}/bin)

add_custom_target(ttt2_kit ALL DEPENDS ${kit_files} ttt2_kit_runtime ttt2-build)

install(DIRECTORY ${TTT2_KIT_DIR}/ DESTINATION lib/ttt2/kit USE_SOURCE_PERMISSIONS)

add_test(NAME ttt2_build_check_test COMMAND ttt2-build check ${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt)
set_tests_properties(ttt2_build_check_test PROPERTIES PASS_REGULAR_EXPRESSION "@error ")

# The check of this computer: the kit links and starts here with this
# build's compiler; a compiler that does not run is refused; and the cases
# of check_test.cmake. (The kit is complete only once ttt2_kit is built.)
set(check_work ${CMAKE_CURRENT_BINARY_DIR}/check_test)
add_test(NAME ttt2_build_system_test
    COMMAND ttt2-build check --compiler ${CMAKE_CXX_COMPILER} --work ${check_work}/system)
set_tests_properties(ttt2_build_system_test PROPERTIES PASS_REGULAR_EXPRESSION "@ok" FAIL_REGULAR_EXPRESSION "@error")
add_test(NAME ttt2_build_compiler_test COMMAND ttt2-build check --compiler /bin/false --work ${check_work}/compiler)
set_tests_properties(ttt2_build_compiler_test PROPERTIES
    PASS_REGULAR_EXPRESSION "@error cannot use the compiler /bin/false: it does not run")
foreach(case masquerade broken_kit)
    add_test(NAME ttt2_build_${case}_test
        COMMAND ${CMAKE_COMMAND} -DCASE=${case} -DBUILD=$<TARGET_FILE:ttt2-build> -DCXX=${CMAKE_CXX_COMPILER}
            -DAR=${CMAKE_AR} -DWORK=${check_work}/${case} -P ${CMAKE_CURRENT_SOURCE_DIR}/check_test.cmake)
endforeach()

# What the hints say per distribution, and where libraries are found.
add_executable(ttt2_build_host_test src/host_test.cpp src/host.cpp src/children.cpp)
target_compile_features(ttt2_build_host_test PRIVATE cxx_std_20)
target_compile_options(ttt2_build_host_test PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME ttt2_build_host_test COMMAND ttt2_build_host_test ${check_work}/host)
