find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
if(WIN32)
    set(CAFE_HOST_LIBS bcrypt ws2_32 winhttp) # digests (host/digest.h), sockets, HTTPS (host/http.h)
else()
    find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
    set(CAFE_HOST_LIBS OpenSSL::Crypto)
endif()

# Linked as objects, not an archive: strong import implementations here must
# replace the weak defaults in the generated imports.cpp, and the export
# registrations must not be dropped by the linker.
add_library(cafe_runtime OBJECT
    src/memory.cpp
    src/loader.cpp
    src/dispatch.cpp
    src/diagnostics.cpp
    src/exports.cpp
    src/sysmem.cpp
    src/os/thread.cpp
    src/os/sync.cpp
    src/os/alarm.cpp
    src/os/heap.cpp
    src/os/dynload.cpp
    src/os/coreinit_misc.cpp
    src/os/guest_format.cpp
    src/os/nn_services.cpp
    src/os/proc_ui.cpp
    src/os/system_config.cpp
    src/os/input.cpp
    src/os/fs.cpp
    src/os/ax.cpp
    src/os/snd_user.cpp
    src/os/network.cpp
    src/os/sockets.cpp
    src/os/sect233.cpp
    src/os/online_identity.cpp
    src/os/act_server.cpp
    src/os/friends.cpp
    src/host/http.cpp
    src/os/swkbd.cpp
    src/os/erreula.cpp
    src/os/dmae.cpp
    src/os/hardware.cpp
    src/os/offline_services.cpp
    src/os/zlib.cpp
    src/vfs.cpp
    src/wua.cpp
    src/gpu/command_processor.cpp
    src/gpu/display.cpp
    src/gpu/null_backend.cpp
    src/gpu/replacements.cpp
    src/gpu/shader_dump.cpp
    src/gpu/tiling.cpp
    src/gpu/vulkan/context.cpp
    src/gpu/vulkan/draw.cpp
    src/gpu/vulkan/formats.cpp
    src/gpu/vulkan/interpolate.cpp
    src/gpu/vulkan/present.cpp
    src/gpu/vulkan/png.cpp
    src/gpu/vulkan/renderer.cpp
    src/gpu/vulkan/targets.cpp
    src/gpu/vulkan/textures.cpp
    src/host/audio.cpp
    src/host/display_settings.cpp
    src/host/keymap.cpp
    src/host/window.cpp
    src/gx2/cbpool.cpp
    src/gx2/display.cpp
    src/gx2/draw.cpp
    src/gx2/format.cpp
    src/gx2/gx2r.cpp
    src/gx2/registers.cpp
    src/gx2/shader.cpp
    src/gx2/state.cpp
    src/gx2/surface.cpp
    src/gx2/texture.cpp)
target_include_directories(cafe_runtime PUBLIC include PRIVATE src)
target_compile_features(cafe_runtime PUBLIC cxx_std_20)
find_package(Vulkan REQUIRED)
find_package(SDL3 REQUIRED CONFIG)
# GLSL -> SPIR-V at run time.
find_library(SHADERC_LIBRARY NAMES shaderc_shared REQUIRED)
target_link_libraries(cafe_runtime PUBLIC cemu_nex cafe_rpx cafe_title ${CAFE_HOST_LIBS} Threads::Threads ZArchive::zarchive amd_addrlib cafe_latte
    Vulkan::Vulkan SDL3::SDL3 ${SHADERC_LIBRARY})
# Vulkan structures are initialised with only their sType.
target_compile_options(cafe_runtime PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

# Tekken Tag Tournament 2's own changes to the game (title/): guest functions
# replaced by address and mid-function hooks, so only the ttt2 target links them.
# install.cpp is its Linux desktop install.
add_library(cafe_ttt2 OBJECT
    src/install.cpp
    src/title/practice.cpp
    src/title/presentation.cpp
    src/title/roster.cpp)
target_link_libraries(cafe_ttt2 PUBLIC cafe_runtime)
target_include_directories(cafe_ttt2 PRIVATE src)
target_compile_options(cafe_ttt2 PRIVATE -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

# The title's file formats and the pictures and texts made from them
# (title/): no runtime, so tests use them on synthetic data.
add_library(cafe_title STATIC
    src/title/archive.cpp
    src/title/draw.cpp
    src/title/font.cpp
    src/title/image.cpp
    src/title/text.cpp
    src/title/title_screen.cpp)
target_include_directories(cafe_title PUBLIC src)
target_compile_features(cafe_title PUBLIC cxx_std_20)
target_link_libraries(cafe_title PUBLIC ZLIB::ZLIB)
target_compile_options(cafe_title PRIVATE -Wall -Wextra -Wpedantic)

# Latte shader decoding and translation, shared by the runtime and tools.
add_library(cafe_latte STATIC
    src/latte/isa.cpp
    src/latte/decode.cpp
    src/latte/disassemble.cpp
    src/latte/environment.cpp
    src/latte/translate.cpp)
target_include_directories(cafe_latte PUBLIC src)
target_compile_features(cafe_latte PUBLIC cxx_std_20)
target_compile_options(cafe_latte PRIVATE -Wall -Wextra -Wpedantic)

add_executable(cafe-shader src/latte/cafe_shader_main.cpp)
target_link_libraries(cafe-shader PRIVATE cafe_latte ${SHADERC_LIBRARY})
target_compile_options(cafe-shader PRIVATE -Wall -Wextra -Wpedantic)
