find_package(ZLIB REQUIRED)
if(WIN32)
    set(CAFE_SHA256_LIB bcrypt)
else()
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    set(CAFE_SHA256_LIB OpenSSL::Crypto)
endif()

# RPX loading is shared with the runtime so both place the image identically.
add_library(cafe_rpx STATIC src/rpx.cpp)
target_include_directories(cafe_rpx PUBLIC src ${CMAKE_SOURCE_DIR}/runtime/include)
target_compile_features(cafe_rpx PUBLIC cxx_std_20)
target_link_libraries(cafe_rpx PUBLIC ZLIB::ZLIB)
target_compile_options(cafe_rpx PRIVATE -Wall -Wextra -Wpedantic)

add_library(cafe_recomp STATIC
    src/ppc.cpp
    src/function_table.cpp
    src/program.cpp
    src/codegen.cpp
    src/text.cpp)
target_link_libraries(cafe_recomp PUBLIC cafe_rpx)
target_compile_options(cafe_recomp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cafe-census src/census.cpp)
target_link_libraries(cafe-census PRIVATE cafe_recomp)
target_compile_options(cafe-census PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cafe-recomp src/recompile_main.cpp)
target_link_libraries(cafe-recomp PRIVATE cafe_recomp ${CAFE_SHA256_LIB})
target_compile_options(cafe-recomp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cafe-semgen src/semgen_main.cpp)
target_link_libraries(cafe-semgen PRIVATE cafe_recomp)
target_compile_options(cafe-semgen PRIVATE -Wall -Wextra -Wpedantic)

add_executable(function_finder_test ${CMAKE_SOURCE_DIR}/tests/function_finder_test.cpp)
target_link_libraries(function_finder_test PRIVATE cafe_recomp)
