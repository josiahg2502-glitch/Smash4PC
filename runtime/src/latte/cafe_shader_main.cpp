// cafe-shader: works on shaders dumped with TTT2_DUMP_SHADERS.
//
//   cafe-shader disasm FILE...           disassemble shader binaries
//   cafe-shader check DIRECTORY          decode every shader in a dump directory
//   cafe-shader translate DIRECTORY [OUT]
//                                        translate and compile the shaders of
//                                        every dumped draw; OUT receives the GLSL

#include "latte/program.h"
#include "latte/translate.h"

#include <shaderc/shaderc.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace {

bool read_words(const std::filesystem::path& path, std::vector<uint32_t>& words) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    const std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    words.assign(bytes.size() / 4, 0);
    std::memcpy(words.data(), bytes.data(), words.size() * 4); // little-endian, as the GPU reads it
    return true;
}

int disasm(int argc, char** argv) {
    int status = 0;
    for (int i = 0; i < argc; ++i) {
        std::vector<uint32_t> words;
        if (!read_words(argv[i], words)) {
            std::fprintf(stderr, "%s: cannot read\n", argv[i]);
            status = 1;
            continue;
        }
        cafe::latte::Program program;
        std::string error;
        std::printf("; %s (%zu bytes)\n", argv[i], words.size() * 4);
        if (!cafe::latte::decode(words, program, error)) {
            std::printf("; decode error: %s\n", error.c_str());
            status = 1;
            continue;
        }
        std::fputs(cafe::latte::disassemble(program).c_str(), stdout);
    }
    return status;
}

int check(const char* directory) {
    std::map<std::string, int> opcodes;
    int shaders = 0, failures = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() != ".bin") continue;
        std::vector<uint32_t> words;
        read_words(entry.path(), words);
        cafe::latte::Program program;
        std::string error;
        ++shaders;
        if (!cafe::latte::decode(words, program, error)) {
            std::printf("%s: %s\n", entry.path().filename().c_str(), error.c_str());
            ++failures;
            continue;
        }
        for (const auto& cf : program.cf) {
            if (cf.kind == cafe::latte::CfInstruction::kNormal) ++opcodes[std::string("CF ") + cafe::latte::isa::cf_name(cf.inst)];
            if (cf.kind == cafe::latte::CfInstruction::kExport) ++opcodes[std::string("CF ") + cafe::latte::isa::cf_export_name(cf.inst)];
            if (cf.kind == cafe::latte::CfInstruction::kAlu) ++opcodes[std::string("CF ") + cafe::latte::isa::cf_alu_name(cf.inst)];
        }
        for (const auto& clause : program.alu_clauses) {
            for (const auto& group : clause) {
                for (const auto& in : group.instructions) ++opcodes[std::string(in.op3 ? "OP3 " : "OP2 ") + in.info->name];
            }
        }
        for (const auto& clause : program.tex_clauses) {
            for (const auto& t : clause) {
                const char* name = cafe::latte::isa::tex::name(t.inst);
                ++opcodes[std::string("TEX ") + (name ? name : "?")];
            }
        }
        for (const auto& clause : program.vtx_clauses) {
            for (const auto& v : clause) ++opcodes[v.inst == 1 ? "VTX SEMANTIC" : "VTX FETCH"];
        }
    }
    std::printf("%d shaders, %d failed to decode\n", shaders, failures);
    for (const auto& [name, count] : opcodes) std::printf("%8d  %s\n", count, name.c_str());
    return failures == 0 ? 0 : 1;
}

// Compiles GLSL to SPIR-V; returns the error text, empty on success.
std::string compile(shaderc_compiler_t compiler, const std::string& glsl, bool vertex, const char* name) {
    shaderc_compile_options_t options = shaderc_compile_options_initialize();
    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_performance);
    shaderc_compilation_result_t result =
        shaderc_compile_into_spv(compiler, glsl.data(), glsl.size(),
                                 vertex ? shaderc_vertex_shader : shaderc_fragment_shader, name, "main", options);
    std::string error;
    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
        error = shaderc_result_get_error_message(result);
    }
    shaderc_result_release(result);
    shaderc_compile_options_release(options);
    return error;
}

int translate_all(const std::filesystem::path& directory, const char* out_dir) {
    shaderc_compiler_t compiler = shaderc_compiler_initialize();
    std::map<std::string, int> errors;
    int draws = 0, failures = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const std::string name = entry.path().filename();
        if (entry.path().extension() != ".regs") continue;
        // draw_<vs>_<ps>_<fs>.regs
        const std::string vs = name.substr(5, 16), ps = name.substr(22, 16), fs = name.substr(39, 16);
        std::vector<uint32_t> regs_file;
        read_words(entry.path(), regs_file);
        std::vector<uint32_t> regs(0x10000, 0);
        if (regs_file.size() == regs.size()) regs = regs_file;
        else std::copy(regs_file.begin(), regs_file.end(), regs.begin() + 0x28000 / 4);
        cafe::latte::Program programs[3];
        bool ok = true;
        const std::string files[3] = {"vs_" + vs, "ps_" + ps, "fs_" + fs};
        for (int i = 0; i < 3; ++i) {
            std::vector<uint32_t> words;
            std::string error;
            if (!read_words(directory / (files[i] + ".bin"), words) || !cafe::latte::decode(words, programs[i], error)) {
                ok = i == 2 && fs == "0000000000000000"; // no fetch shader
                if (!ok) std::printf("%s: cannot decode %s %s\n", name.c_str(), files[i].c_str(), error.c_str());
            }
        }
        if (!ok) continue;
        ++draws;
        for (int stage = 0; stage < 2; ++stage) {
            cafe::latte::ShaderEnvironment env;
            cafe::latte::build_environment(regs.data(), stage == 0 ? cafe::latte::Stage::kVertex : cafe::latte::Stage::kPixel, env);
            cafe::latte::TranslatedShader shader;
            std::string error;
            const bool translated = cafe::latte::translate(programs[stage], stage == 0 ? &programs[2] : nullptr, env, shader, error);
            const std::string label = files[stage] + " (" + name + ")";
            if (out_dir) {
                std::ofstream(std::filesystem::path(out_dir) / (files[stage] + "_" + name.substr(5, 50) + ".glsl")) << shader.glsl;
            }
            if (!translated) {
                std::printf("%s: translate: %s\n", label.c_str(), error.c_str());
                ++errors["translate: " + error];
                ++failures;
                continue;
            }
            const std::string compile_error = compile(compiler, shader.glsl, stage == 0, label.c_str());
            if (!compile_error.empty()) {
                std::printf("%s: compile:\n%s\n", label.c_str(), compile_error.c_str());
                ++errors["compile"];
                ++failures;
            }
        }
    }
    shaderc_compiler_release(compiler);
    std::printf("%d draws, %d shader failures\n", draws, failures);
    for (const auto& [message, count] : errors) std::printf("%6d  %s\n", count, message.c_str());
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::strcmp(argv[1], "disasm") == 0) return disasm(argc - 2, argv + 2);
    if (argc == 3 && std::strcmp(argv[1], "check") == 0) return check(argv[2]);
    if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "translate") == 0) {
        return translate_all(argv[2], argc == 4 ? argv[3] : nullptr);
    }
    std::fprintf(stderr, "usage: %s disasm FILE... | check DIRECTORY | translate DIRECTORY [OUT]\n", argv[0]);
    return 2;
}
