// Draws: shaders (translated and compiled on first use), pipelines, the
// per-draw constants and resources, index conversion, and the draw.

#include "gpu/vulkan/renderer.h"

#include "gpu/latte.h"
#include "gx2/types.h"

#include "cafe/runtime.h"

#include <shaderc/shaderc.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace cafe::gpu::vk {
namespace {

using namespace latte;

uint64_t fnv(const void* data, size_t size, uint64_t h = 0xCBF29CE484222325ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

void report_once(const char* what, uint32_t value) {
    static std::vector<std::pair<std::string, uint32_t>> seen;
    for (const auto& s : seen) {
        if (s.first == what && s.second == value) return;
    }
    seen.emplace_back(what, value);
    std::fprintf(stderr, "ttt2: gpu: %s (0x%X)\n", what, value);
}

VkBlendFactor blend_factor(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 2: return VK_BLEND_FACTOR_SRC_COLOR;
    case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_DST_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 13: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 15: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 16: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 18: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 20: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default:
        report_once("unsupported blend factor", f);
        return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp blend_op(uint32_t f) {
    switch (f) {
    case 0: return VK_BLEND_OP_ADD;
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}

bool is_integer(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_UINT:
    case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return true;
    default: return false;
    }
}

bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

std::filesystem::path g_cache_override;

// Compiled shaders and pipelines: set_cache_directory's, else
// $XDG_CACHE_HOME/ttt2, ~/.cache/ttt2 or %LOCALAPPDATA%/ttt2.
std::filesystem::path cache_directory() {
    static const std::filesystem::path dir = [] {
        std::filesystem::path d = g_cache_override;
        if (!d.empty()) {
        } else if (const char* x = std::getenv("XDG_CACHE_HOME"); x && *x) {
            d = std::filesystem::path(x) / "ttt2";
        } else if (const char* h = std::getenv("HOME"); h && *h) {
            d = std::filesystem::path(h) / ".cache" / "ttt2";
        } else if (const char* l = std::getenv("LOCALAPPDATA"); l && *l) {
            d = std::filesystem::path(l) / "ttt2";
        } else {
            return std::filesystem::path();
        }
        std::error_code ec;
        std::filesystem::create_directories(d / "spirv", ec);
        return ec ? std::filesystem::path() : d;
    }();
    return dir;
}

std::vector<uint32_t> load_cached_spirv(uint64_t key) {
    std::vector<uint32_t> code;
    if (cache_directory().empty()) return code;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(key));
    std::ifstream in(cache_directory() / "spirv" / name, std::ios::binary | std::ios::ate);
    if (!in) return code;
    const std::streamsize size = in.tellg();
    if (size <= 0 || size % 4) return code;
    code.resize(static_cast<size_t>(size) / 4);
    in.seekg(0);
    in.read(reinterpret_cast<char*>(code.data()), size);
    if (!in || code[0] != 0x07230203u) code.clear(); // SPIR-V magic
    return code;
}

void store_cached_spirv(uint64_t key, const std::vector<uint32_t>& code) {
    if (cache_directory().empty()) return;
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.spv", static_cast<unsigned long long>(key));
    const std::filesystem::path path = cache_directory() / "spirv" / name;
    std::ofstream(path.string() + ".tmp", std::ios::binary)
        .write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(code.size() * 4));
    std::error_code ec;
    std::filesystem::rename(path.string() + ".tmp", path, ec);
}

// Lists of what earlier runs compiled, for precompile(): records appended as
// shaders and pipelines are first made, after a header naming the record
// size (a list written by another layout is ignored).
struct SavedShader {
    uint64_t glsl_hash;
    uint32_t stage;
    uint32_t texture_mask;
    uint32_t uses_registers;
    uint32_t reserved;
};
constexpr uint32_t kListMagic = 0x53344331; // "S4C1"

template <typename T>
void append_saved(const char* name, const T& record) {
    if (cache_directory().empty()) return;
    const std::filesystem::path path = cache_directory() / name;
    std::error_code ec;
    const bool fresh = !std::filesystem::exists(path, ec);
    std::ofstream out(path, std::ios::binary | std::ios::app);
    if (fresh) {
        const uint32_t header[2] = {kListMagic, sizeof(T)};
        out.write(reinterpret_cast<const char*>(header), sizeof header);
    }
    out.write(reinterpret_cast<const char*>(&record), sizeof record);
}

template <typename T>
std::vector<T> load_saved(const char* name) {
    std::vector<T> records;
    if (cache_directory().empty()) return records;
    std::ifstream in(cache_directory() / name, std::ios::binary);
    uint32_t header[2] = {};
    if (!in.read(reinterpret_cast<char*>(header), sizeof header) || header[0] != kListMagic || header[1] != sizeof(T)) {
        return records;
    }
    T record;
    while (in.read(reinterpret_cast<char*>(&record), sizeof record)) records.push_back(record);
    return records;
}

const char* dump_directory() {
    static const char* dir = [] {
        const char* d = std::getenv("TTT2_DUMP_SHADERS");
        return (d != nullptr && *d != '\0') ? d : nullptr;
    }();
    return dir;
}

} // namespace

// ------------------------------------------------------- pipeline cache

} // namespace cafe::gpu::vk

namespace cafe::gpu {
void set_cache_directory(const std::filesystem::path& directory) { vk::g_cache_override = directory; }
} // namespace cafe::gpu

namespace cafe::gpu::vk {

void Renderer::load_pipeline_cache() {
    std::vector<char> data;
    if (!cache_directory().empty()) {
        std::ifstream in(cache_directory() / "pipelines.bin", std::ios::binary | std::ios::ate);
        if (in && in.tellg() > 0) {
            data.resize(static_cast<size_t>(in.tellg()));
            in.seekg(0);
            in.read(data.data(), static_cast<std::streamsize>(data.size()));
        }
    }
    VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    pci.initialDataSize = data.size();
    pci.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(ctx_.device, &pci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        pci.initialDataSize = 0; // stale or from another driver
        pci.pInitialData = nullptr;
        VK_CHECK(vkCreatePipelineCache(ctx_.device, &pci, nullptr, &pipeline_cache_));
    }
}

// On a thread of its own: the cache is large, and reading it out and writing
// it takes long enough to stall a frame (the cache is internally synchronized).
void Renderer::save_pipeline_cache() {
    static std::atomic<bool> saving{false};
    if (new_pipelines_ == 0 || cache_directory().empty() || saving.exchange(true)) return;
    new_pipelines_ = 0;
    std::thread([this] {
        size_t size = 0;
        if (vkGetPipelineCacheData(ctx_.device, pipeline_cache_, &size, nullptr) == VK_SUCCESS && size != 0) {
            std::vector<char> data(size);
            if (vkGetPipelineCacheData(ctx_.device, pipeline_cache_, &size, data.data()) == VK_SUCCESS) {
                const std::filesystem::path path = cache_directory() / "pipelines.bin";
                std::ofstream(path.string() + ".tmp", std::ios::binary).write(data.data(), static_cast<std::streamsize>(size));
                std::error_code ec;
                std::filesystem::rename(path.string() + ".tmp", path, ec);
            }
        }
        saving = false;
    }).detach();
}

// ---------------------------------------------------------------- shaders

const Program* Renderer::program(uint32_t address, uint32_t size, uint64_t& hash) {
    const uint64_t key = (uint64_t{address} << 32) | size;
    auto it = programs_.find(key);
    if (it == programs_.end()) {
        ProgramEntry e{0, {}, false};
        if (size != 0 && guest_memory_committed(address, size)) {
            std::vector<uint32_t> words(size / 4);
            std::memcpy(words.data(), guest_pointer(address), words.size() * 4); // little-endian, the GPU's order
            e.hash = fnv(words.data(), words.size() * 4);
            std::string error;
            e.ok = decode(words, e.program, error);
            if (!e.ok) std::fprintf(stderr, "ttt2: gpu: shader at 0x%08X: %s\n", address, error.c_str());
        }
        it = programs_.emplace(key, std::move(e)).first;
        if (size != 0) {
            for_pages(address, uint64_t{address} + size, [&](uint32_t page) {
                std::vector<uint64_t>& keys = program_pages_[page];
                if (std::find(keys.begin(), keys.end(), key) == keys.end()) keys.push_back(key);
            });
        }
    }
    hash = it->second.hash;
    return it->second.ok ? &it->second.program : nullptr;
}

const ShaderModule* Renderer::shader(Stage stage, uint32_t address, uint32_t size, uint32_t fetch_address,
                                     uint32_t fetch_size, const ShaderEnvironment& env) {
    uint64_t program_hash = 0, fetch_hash = 0;
    const Program* p = program(address, size, program_hash);
    if (p == nullptr) return nullptr;
    const Program* fetch = nullptr;
    if (stage == Stage::kVertex && fetch_address != 0) fetch = program(fetch_address, fetch_size, fetch_hash);
    const uint64_t keys[3] = {program_hash, fetch_hash, environment_key(env)};
    const uint64_t key = fnv(keys, sizeof(keys));
    auto& entry = shaders_[key];
    if (entry) {
        if (entry->module == VK_NULL_HANDLE && !entry->failed) resolve_module(*entry);
        return entry->failed ? nullptr : entry.get();
    }
    entry = std::make_unique<ShaderModule>();
    const char* kind = stage == Stage::kVertex ? "vs" : "ps";
    std::string error;
    bool translated;
    {
        Timed timed{spent_translate_};
        translated = translate(*p, fetch, env, entry->info, error);
    }
    if (!translated) {
        std::fprintf(stderr, "ttt2: gpu: cannot translate %s %016llx: %s\n", kind,
                     static_cast<unsigned long long>(program_hash), error.c_str());
        entry->failed = true;
        return nullptr;
    }
    if (const char* dir = dump_directory()) {
        char name[128];
        std::snprintf(name, sizeof(name), "%s_%016llx_%016llx.glsl", kind, static_cast<unsigned long long>(program_hash),
                      static_cast<unsigned long long>(key));
        std::ofstream(std::filesystem::path(dir) / name) << entry->info.glsl;
    }
    // SPIR-V is cached on disk by the GLSL it came from, and modules are
    // shared by it (precompile() makes those of earlier runs).
    const uint64_t glsl_hash = fnv(entry->info.glsl.data(), entry->info.glsl.size());
    entry->glsl_hash = glsl_hash;
    if (auto it = modules_.find(glsl_hash); it != modules_.end()) {
        entry->module = it->second;
        return entry.get();
    }
    if (async_compile_) {
        entry->stage_vertex = stage == Stage::kVertex;
        if (!pending_modules_.count(glsl_hash)) {
            auto pending = std::make_shared<PendingModule>();
            pending_modules_[glsl_hash] = pending;
            compile_async([this, pending, glsl = entry->info.glsl, glsl_hash, vertex = stage == Stage::kVertex,
                           program_hash] {
                std::vector<uint32_t> code = load_cached_spirv(glsl_hash);
                if (code.empty()) {
                    // shaderc compilers are per thread.
                    thread_local shaderc_compiler_t compiler = shaderc_compiler_initialize();
                    shaderc_compile_options_t options = shaderc_compile_options_initialize();
                    shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan,
                                                           shaderc_env_version_vulkan_1_3);
                    shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
                    shaderc_compilation_result_t result = shaderc_compile_into_spv(
                        compiler, glsl.data(), glsl.size(), vertex ? shaderc_vertex_shader : shaderc_fragment_shader,
                        vertex ? "vs" : "ps", "main", options);
                    shaderc_compile_options_release(options);
                    if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
                        std::fprintf(stderr, "ttt2: gpu: cannot compile %s %016llx:\n%s\n", vertex ? "vs" : "ps",
                                     static_cast<unsigned long long>(program_hash),
                                     shaderc_result_get_error_message(result));
                        shaderc_result_release(result);
                        pending->state = Built::kFailed;
                        return;
                    }
                    const auto* words = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
                    code.assign(words, words + shaderc_result_get_length(result) / 4);
                    shaderc_result_release(result);
                    store_cached_spirv(glsl_hash, code);
                }
                VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
                mci.codeSize = code.size() * 4;
                mci.pCode = code.data();
                VK_CHECK(vkCreateShaderModule(ctx_.device, &mci, nullptr, &pending->module));
                pending->state = Built::kReady;
            });
        }
        resolve_module(*entry);
        return entry->failed ? nullptr : entry.get();
    }
    std::vector<uint32_t> spirv = load_cached_spirv(glsl_hash);
    if (spirv.empty()) {
        shaderc_compile_options_t options = shaderc_compile_options_initialize();
        shaderc_compile_options_set_target_env(options, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_3);
        // The driver optimises; SPIR-V optimisation only costs compile time.
        shaderc_compile_options_set_optimization_level(options, shaderc_optimization_level_zero);
        Timed timed{spent_compile_};
        shaderc_compilation_result_t result = shaderc_compile_into_spv(
            compiler_, entry->info.glsl.data(), entry->info.glsl.size(),
            stage == Stage::kVertex ? shaderc_vertex_shader : shaderc_fragment_shader, kind, "main", options);
        shaderc_compile_options_release(options);
        if (shaderc_result_get_compilation_status(result) != shaderc_compilation_status_success) {
            std::fprintf(stderr, "ttt2: gpu: cannot compile %s %016llx:\n%s\n", kind,
                         static_cast<unsigned long long>(program_hash), shaderc_result_get_error_message(result));
            shaderc_result_release(result);
            entry->failed = true;
            return nullptr;
        }
        const auto* code = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
        spirv.assign(code, code + shaderc_result_get_length(result) / 4);
        shaderc_result_release(result);
        store_cached_spirv(glsl_hash, spirv);
    }
    entry->module = cached_module(glsl_hash, &spirv);
    append_saved("shaders.list", SavedShader{glsl_hash, stage == Stage::kVertex ? 0u : 1u, entry->info.texture_mask,
                                             entry->info.uses_registers ? 1u : 0u, 0});
    return entry.get();
}

// Takes `entry`'s module from a finished worker compile, if any.
void Renderer::resolve_module(ShaderModule& entry) {
    if (auto it = modules_.find(entry.glsl_hash); it != modules_.end()) {
        entry.module = it->second;
        return;
    }
    const auto pending = pending_modules_.find(entry.glsl_hash);
    if (pending == pending_modules_.end()) return;
    switch (pending->second->state.load()) {
    case Built::kPending: return;
    case Built::kFailed: entry.failed = true; return;
    case Built::kReady:
        entry.module = modules_[entry.glsl_hash] = pending->second->module;
        append_saved("shaders.list", SavedShader{entry.glsl_hash, entry.stage_vertex ? 0u : 1u, entry.info.texture_mask,
                                                 entry.info.uses_registers ? 1u : 0u, 0});
        pending_modules_.erase(pending);
        return;
    }
}

void Renderer::compile_async(std::function<void()> job) {
    if (workers_.empty()) {
        const unsigned count = std::max(2u, std::thread::hardware_concurrency() / 2);
        for (unsigned i = 0; i < count; ++i) {
            workers_.emplace_back([this] {
                for (;;) {
                    std::function<void()> next;
                    {
                        std::unique_lock lock(jobs_mutex_);
                        jobs_cv_.wait(lock, [&] { return !jobs_.empty(); });
                        next = std::move(jobs_.front());
                        jobs_.pop_front();
                    }
                    next();
                }
            });
            workers_.back().detach();
        }
    }
    {
        std::lock_guard lock(jobs_mutex_);
        jobs_.push_back(std::move(job));
    }
    jobs_cv_.notify_one();
}

// The module of `glsl_hash`, made from `spirv` or the disk cache; null if
// neither has it.
VkShaderModule Renderer::cached_module(uint64_t glsl_hash, const std::vector<uint32_t>* spirv) {
    if (auto it = modules_.find(glsl_hash); it != modules_.end()) return it->second;
    std::vector<uint32_t> loaded;
    if (spirv == nullptr) {
        loaded = load_cached_spirv(glsl_hash);
        if (loaded.empty()) return VK_NULL_HANDLE;
        spirv = &loaded;
    }
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = spirv->size() * 4;
    mci.pCode = spirv->data();
    VkShaderModule module = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(ctx_.device, &mci, nullptr, &module));
    modules_[glsl_hash] = module;
    return module;
}

// -------------------------------------------------------------- pipelines

VkPipelineLayout Renderer::pipeline_layout(uint32_t vs_textures, uint32_t ps_textures, bool vs_registers,
                                           bool ps_registers) {
    const uint64_t key = uint64_t{vs_textures} | (uint64_t{ps_textures} << 18) | (uint64_t{vs_registers} << 36) |
                         (uint64_t{ps_registers} << 37);
    if (auto it = layouts_.find(key); it != layouts_.end()) return it->second;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.push_back({abi::kDrawConstantsBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                        VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    if (vs_registers) bindings.push_back({abi::kVsRegistersBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
    if (ps_registers) bindings.push_back({abi::kPsRegistersBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
    for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
        if (vs_textures & (1u << slot)) {
            bindings.push_back({abi::kVsTextureBinding + slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr});
        }
        if (ps_textures & (1u << slot)) {
            bindings.push_back({abi::kPsTextureBinding + slot, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
    }
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dci.bindingCount = static_cast<uint32_t>(bindings.size());
    dci.pBindings = bindings.data();
    VkDescriptorSetLayout set_layout;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx_.device, &dci, nullptr, &set_layout));
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &set_layout;
    VkPipelineLayout layout;
    VK_CHECK(vkCreatePipelineLayout(ctx_.device, &lci, nullptr, &layout));
    layouts_[key] = layout;
    return layout;
}

const Pipeline* Renderer::pipeline(const Registers& regs, const ShaderModule* vs, const ShaderModule* ps,
                                   const VkFormat* colors, uint32_t color_count, VkFormat depth, uint32_t topology_class) {
    PipelineKey k;
    std::memset(&k, 0, sizeof(k));
    k.vs_glsl = vs->glsl_hash;
    k.ps_glsl = ps->glsl_hash;
    k.color_count = color_count;
    k.depth = depth;
    const uint32_t color_control = regs[reg::CB_COLOR_CONTROL];
    const uint32_t blend_enable = (color_control >> 8) & 0xFF;
    const bool per_mrt = (color_control >> 7) & 1;
    k.write_mask = regs[reg::CB_TARGET_MASK];
    for (uint32_t i = 0; i < color_count; ++i) {
        k.colors[i] = colors[i];
        if (colors[i] == VK_FORMAT_UNDEFINED) continue;
        if ((blend_enable >> i) & 1 && !is_integer(colors[i])) {
            k.blend[i] = (regs[reg::CB_BLEND0_CONTROL + (per_mrt ? i * 4 : 0)] & 0x7FFFFFFF) | 0x80000000u;
        }
    }
    const uint32_t mode = regs[reg::PA_SU_SC_MODE_CNTL];
    if ((mode >> 3) & 3) k.polygon_mode = (mode >> 5) & 7; // front polygon type: 0 points, 1 lines, 2 fill
    else k.polygon_mode = 2;
    k.topology_class = topology_class;
    const uint32_t clip = regs[reg::PA_CL_CLIP_CNTL];
    k.depth_clamp = ((clip >> 26) & 3) == 3; // ZCLIP_NEAR/FAR_DISABLE
    if ((color_control >> 16 & 0xFF) != 0xCC) report_once("unsupported ROP3", (color_control >> 16) & 0xFF);

    const uint64_t key = fnv(&k, sizeof(k));
    auto& entry = pipelines_[key];
    if (entry) {
        const Built state = entry->state.load();
        if (state == Built::kFailed) return nullptr;
        if (state == Built::kReady && !entry->listed) {
            entry->listed = true;
            append_saved("pipelines.list", entry->key);
        }
        return entry.get(); // a pending one: the draw waits for it
    }
    entry = std::make_unique<Pipeline>();
    entry->vs = vs;
    entry->ps = ps;
    entry->key = k;
    entry->layout = pipeline_layout(vs->info.texture_mask, ps->info.texture_mask, vs->info.uses_registers,
                                    ps->info.uses_registers);
    if (async_compile_) {
        entry->state = Built::kPending;
        compile_async([this, p = entry.get(), vm = vs->module, pm = ps->module] {
            p->pipeline = build_pipeline(p->key, vm, pm, p->layout);
            ++new_pipelines_;
            p->state = p->pipeline != VK_NULL_HANDLE ? Built::kReady : Built::kFailed;
        });
        return entry.get();
    }
    {
        Timed timed{spent_pipeline_};
        entry->pipeline = build_pipeline(k, vs->module, ps->module, entry->layout);
    }
    ++new_pipelines_;
    if (entry->pipeline == VK_NULL_HANDLE) {
        entry->state = Built::kFailed;
        return nullptr;
    }
    entry->listed = true;
    append_saved("pipelines.list", k);
    return entry.get();
}

VkPipeline Renderer::build_pipeline(const PipelineKey& k, VkShaderModule vs, VkShaderModule ps,
                                    VkPipelineLayout layout) {
    const uint32_t color_count = k.color_count;
    const VkFormat* colors = k.colors;
    const VkFormat depth = k.depth;
    const uint32_t topology_class = k.topology_class;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topology_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST
                  : topology_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                        : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.polygon_mode == 0 ? VK_POLYGON_MODE_POINT : k.polygon_mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.depthClampEnable = k.depth_clamp ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachments[8]{};
    for (uint32_t i = 0; i < color_count; ++i) {
        VkPipelineColorBlendAttachmentState& a = attachments[i];
        a.colorWriteMask = colors[i] == VK_FORMAT_UNDEFINED ? 0 : (k.write_mask >> (4 * i)) & 0xF;
        if (k.blend[i]) {
            const uint32_t b = k.blend[i];
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = blend_factor(b & 0x1F);
            a.colorBlendOp = blend_op((b >> 5) & 7);
            a.dstColorBlendFactor = blend_factor((b >> 8) & 0x1F);
            if ((b >> 29) & 1) {
                a.srcAlphaBlendFactor = blend_factor((b >> 16) & 0x1F);
                a.alphaBlendOp = blend_op((b >> 21) & 7);
                a.dstAlphaBlendFactor = blend_factor((b >> 24) & 0x1F);
            } else {
                a.srcAlphaBlendFactor = a.srcColorBlendFactor;
                a.alphaBlendOp = a.colorBlendOp;
                a.dstAlphaBlendFactor = a.dstColorBlendFactor;
            }
        }
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = color_count;
    cb.pAttachments = attachments;
    static const VkDynamicState kDynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
    };
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = static_cast<uint32_t>(std::size(kDynamic));
    dyn.pDynamicStates = kDynamic;
    VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ri.colorAttachmentCount = color_count;
    ri.pColorAttachmentFormats = colors;
    ri.depthAttachmentFormat = depth;
    ri.stencilAttachmentFormat = has_stencil(depth) ? depth : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo gci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gci.pNext = &ri;
    gci.stageCount = 2;
    gci.pStages = stages;
    gci.pVertexInputState = &vi;
    gci.pInputAssemblyState = &ia;
    gci.pViewportState = &vp;
    gci.pRasterizationState = &rs;
    gci.pMultisampleState = &ms;
    gci.pDepthStencilState = &ds;
    gci.pColorBlendState = &cb;
    gci.pDynamicState = &dyn;
    gci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    const VkResult r = vkCreateGraphicsPipelines(ctx_.device, pipeline_cache_, 1, &gci, nullptr, &pipeline);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "ttt2: gpu: pipeline creation failed (%d)\n", static_cast<int>(r));
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

// Everything earlier runs compiled, before the title draws: the modules
// from their SPIR-V, then the pipelines on every core.
void Renderer::precompile() {
    const auto started = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, SavedShader> shaders;
    for (const SavedShader& s : load_saved<SavedShader>("shaders.list")) shaders[s.glsl_hash] = s;
    struct Job {
        PipelineKey key;
        VkShaderModule vs, ps;
        VkPipelineLayout layout;
        VkPipeline pipeline;
    };
    std::vector<Job> jobs;
    for (const PipelineKey& k : load_saved<PipelineKey>("pipelines.list")) {
        const uint64_t key = fnv(&k, sizeof(k));
        if (pipelines_.count(key)) continue;
        const auto vs = shaders.find(k.vs_glsl), ps = shaders.find(k.ps_glsl);
        if (vs == shaders.end() || ps == shaders.end()) continue;
        const VkShaderModule vm = cached_module(k.vs_glsl, nullptr), pm = cached_module(k.ps_glsl, nullptr);
        if (vm == VK_NULL_HANDLE || pm == VK_NULL_HANDLE) continue;
        const VkPipelineLayout layout = pipeline_layout(vs->second.texture_mask, ps->second.texture_mask,
                                                        vs->second.uses_registers != 0, ps->second.uses_registers != 0);
        pipelines_[key] = std::make_unique<Pipeline>(); // claims the key: duplicates in the list are skipped
        jobs.push_back({k, vm, pm, layout, VK_NULL_HANDLE});
    }
    if (jobs.empty()) return;
    std::fprintf(stderr, "ttt2: gpu: precompiling %zu pipelines (%zu shaders)\n", jobs.size(), modules_.size());
    std::atomic<size_t> next{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency()); ++t) {
        threads.emplace_back([&] {
            for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
                jobs[i].pipeline = build_pipeline(jobs[i].key, jobs[i].vs, jobs[i].ps, jobs[i].layout);
            }
        });
    }
    for (auto& thread : threads) thread.join();
    for (const Job& j : jobs) {
        Pipeline& p = *pipelines_[fnv(&j.key, sizeof(j.key))];
        p.pipeline = j.pipeline;
        p.layout = j.layout;
        p.key = j.key;
        p.listed = true; // already in pipelines.list
        p.state = j.pipeline != VK_NULL_HANDLE ? Built::kReady : Built::kFailed;
    }
    std::fprintf(stderr, "ttt2: gpu: precompiled in %.1f s\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
}

// ----------------------------------------------------------------- draws

abi::Buffer Renderer::buffer_descriptor(uint32_t address, uint32_t size, uint32_t stride) const {
    uint32_t available = 0;
    const VkDeviceAddress device = address != 0 ? guest_.device_address(address, available) : 0;
    if (device == 0 || size == 0) return {0, 0, 0, stride};
    return {static_cast<uint32_t>(device), static_cast<uint32_t>(device >> 32), std::min(size, available), stride};
}

namespace {
// Bytes of a constant block to copy for interpolation: what the shader
// reads; a block it indexes freely only whole and up to 256 KiB (0: read
// it in place).
uint32_t read_extent(uint32_t size, uint32_t extent) {
    if (extent != UINT32_MAX) return std::min(size, extent);
    return size <= (256u << 10) ? size : 0;
}
} // namespace

void Renderer::draw(const Registers& regs, const Draw& d) {
    if (!replaying_) ++draws_;
    const auto r = [&](uint32_t address) { return regs[address]; };
    // Interpolation: every draw of a frame and of its replays, in order.
    const uint32_t draw_index = frames_per_frame_ > 1 ? begin_draw_record(regs, d) : 0;
    const uint32_t instances = std::max<uint32_t>(d.num_instances, 1);

    // Primitive type: Vulkan topology, or a conversion through indices.
    enum class Convert { kNone, kQuads, kQuadStrip, kLoop } convert = Convert::kNone;
    VkPrimitiveTopology topology;
    uint32_t topology_class = 2;
    // shortcut: geometry shaders (ES -> GS -> copy shader) are not translated; the draw runs the copy shader as its
    // vertex shader. Translate the GS stage if a title's GS draws matter.
    if (r(reg::VGT_GS_MODE) & 3) report_once("geometry shader draw (GS stage not translated)", r(reg::VGT_GS_MODE));
    if (r(reg::VGT_HOS_CNTL) & 3 && r(reg::VGT_PRIMITIVE_TYPE) >= 0x80) report_once("tessellated draw", r(reg::VGT_PRIMITIVE_TYPE));
    switch (r(reg::VGT_PRIMITIVE_TYPE)) {
    case 0x01: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; topology_class = 0; break;
    case 0x02: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; topology_class = 1; break;
    case 0x03: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; topology_class = 1; break;
    case 0x12: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; topology_class = 1; convert = Convert::kLoop; break;
    case 0x04: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 0x05: case 0x15: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 0x06: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 0x13: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; convert = Convert::kQuads; break;
    case 0x14: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; convert = Convert::kQuadStrip; break;
    default:
        report_once("unsupported primitive type", r(reg::VGT_PRIMITIVE_TYPE));
        ++skipped_draws_;
        return;
    }
    if (d.use_opaque) {
        report_once("unsupported stream-out draw", 0);
        ++skipped_draws_;
        return;
    }
    if (d.count == 0) return;
    // TTT2_SKIP_VS=<hash>: draws with that vertex shader (as the trace prints it) are not drawn, to see what they do.
    if (static const char* skip = std::getenv("TTT2_SKIP_VS"); skip) {
        uint64_t vh = 0;
        program(r(reg::SQ_PGM_START_VS) << 8, r(reg::SQ_PGM_START_VS + 4) << 3, vh);
        if (vh == std::strtoull(skip, nullptr, 16)) return;
    }

    // Render targets.
    std::array<const Target*, 8> colors{};
    uint32_t color_count = 0;
    const uint32_t target_mask = r(reg::CB_TARGET_MASK);
    for (uint32_t t = 0; t < 8; ++t) {
        const uint32_t info = r(reg::CB_COLOR0_INFO + t * 4), base = r(reg::CB_COLOR0_BASE + t * 4);
        if (((target_mask >> (4 * t)) & 0xF) == 0 || ((info >> 2) & 0x3F) == 0 || base == 0) continue;
        colors[t] = color_target(base, r(reg::CB_COLOR0_SIZE + t * 4), info);
        if (colors[t]) color_count = t + 1;
    }
    const uint32_t depth_control = r(reg::DB_DEPTH_CONTROL);
    const Target* depth = nullptr;
    if ((depth_control & 7) != 0 && (r(reg::DB_DEPTH_INFO) & 7) != 0 && r(reg::DB_DEPTH_BASE) != 0) {
        depth = depth_target(r(reg::DB_DEPTH_BASE), r(reg::DB_DEPTH_SIZE), r(reg::DB_DEPTH_INFO));
    }
    // A pass is clipped to its smallest attachment, the console's GPU is not:
    // a title-sized target drawn with upscaled ones is upscaled too, and an
    // attachment smaller than the others is enlarged, or all but their
    // top-left corner is lost (Smash's effects: a 960x544 buffer drawn with
    // a 512x272 mask as color1).
    if (!replaying_) {
        float top = depth ? depth->scale : 1;
        for (uint32_t i = 0; i < color_count; ++i) top = colors[i] ? std::max(top, colors[i]->scale) : top;
        const auto promote = [&](const Target* t) {
            if (t && t->scale < top && !t->write_back) refit_target(*const_cast<Target*>(t), top);
        };
        for (uint32_t i = 0; i < color_count; ++i) promote(colors[i]);
        promote(depth);
        uint32_t width = 0, height = 0;
        const auto measure = [&](const Target* t) {
            if (t) width = std::max(width, t->image.width), height = std::max(height, t->image.height);
        };
        for (uint32_t i = 0; i < color_count; ++i) measure(colors[i]);
        measure(depth);
        // Only as far as the scissor reaches: what the draw can write.
        const uint32_t br = r(reg::PA_SC_GENERIC_SCISSOR_TL + 4), vbr = r(reg::PA_SC_VPORT_SCISSOR_0_TL + 4);
        width = std::min(width, static_cast<uint32_t>(std::ceil(std::min(br & 0x7FFF, vbr & 0x7FFF) * top)));
        height = std::min(height, static_cast<uint32_t>(std::ceil(std::min((br >> 16) & 0x7FFF, (vbr >> 16) & 0x7FFF) * top)));
        const auto enlarge = [&](const Target* t) {
            if (t && t->scale == top && !t->write_back && (t->image.width < width || t->image.height < height)) {
                refit_target(*const_cast<Target*>(t), top, width, height);
            }
        };
        for (uint32_t i = 0; i < color_count; ++i) enlarge(colors[i]);
        enlarge(depth);
    }
    const bool rasterizer_discard = (r(reg::PA_CL_CLIP_CNTL) >> 22) & 1;
    const bool stream_out = r(reg::VGT_STRMOUT_EN) & 1;
    if (color_count == 0 && depth == nullptr && !stream_out) {
        // Nothing to write (every target masked off): no effect, nothing lost.
        return;
    }

    // Shaders.
    ShaderEnvironment vs_env, ps_env;
    build_environment(regs.value, Stage::kVertex, vs_env);
    build_environment(regs.value, Stage::kPixel, ps_env);
    const ShaderModule* vs = shader(Stage::kVertex, r(reg::SQ_PGM_START_VS) << 8, r(reg::SQ_PGM_START_VS + 4) << 3,
                                    r(reg::SQ_PGM_START_FS) << 8, r(reg::SQ_PGM_START_FS + 4) << 3, vs_env);
    const ShaderModule* ps = shader(Stage::kPixel, r(reg::SQ_PGM_START_PS) << 8, r(reg::SQ_PGM_START_PS + 4) << 3, 0, 0,
                                    ps_env);
    if (vs == nullptr || ps == nullptr) {
        report_once(vs == nullptr ? "draw skipped: vertex shader failed, at" : "draw skipped: pixel shader failed, at",
                    vs == nullptr ? r(reg::SQ_PGM_START_VS) << 8 : r(reg::SQ_PGM_START_PS) << 8);
        ++skipped_draws_;
        return;
    }

    // Textures, before the rendering pass: they may upload or copy.
    struct Bound {
        VkImageView view;
        VkSampler sampler;
        bool scaled;
    };
    Bound vs_textures[abi::kTextureSlots]{}, ps_textures[abi::kTextureSlots]{};
    const auto bind_textures = [&](const ShaderModule* s, uint32_t resource_base, uint32_t sampler_base,
                                   uint32_t border_base, Bound* out) {
        for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
            if (!(s->info.texture_mask & (1u << slot))) continue;
            uint32_t words[7];
            for (int i = 0; i < 7; ++i) words[i] = r(kResourceBase + ((resource_base + slot) * resource::kWords + i) * 4);
            uint32_t sampler_words[3];
            for (int i = 0; i < 3; ++i) sampler_words[i] = r(kSamplerBase + ((sampler_base + slot) * sampler::kWords + i) * 4);
            float border[4];
            for (int i = 0; i < 4; ++i) border[i] = std::bit_cast<float>(r(border_base + slot * 16 + i * 4));
            const bool valid = (words[6] >> 30) == (resource::kTypeValidTexture >> 30);
            if (!valid) {
                // Nothing bound (the shader samples it on a path it does not
                // take, or the title relies on it): the GPU reads zeros, and
                // the shader declared a 2D float texture. (Fallen Garden's
                // water has such a slot; skipping the draw left no water.)
                out[slot] = {null_texture().view, sampler(sampler_words, border, (s->info.shadow_mask >> slot) & 1), false};
                continue;
            }
            const Texture* t = texture(words);
            if (t == nullptr) {
                report_once("draw skipped: texture could not be created, format", words[1] >> 26);
                return false;
            }
            out[slot] = {t->view, sampler(sampler_words, border, (s->info.shadow_mask >> slot) & 1), t->scale > 1};
        }
        return true;
    };
    if (!bind_textures(vs, resource::kVsTexture, sampler::kVs, reg::TD_VS_SAMPLER_BORDER0_RED, vs_textures) ||
        !bind_textures(ps, resource::kPsTexture, sampler::kPs, reg::TD_PS_SAMPLER_BORDER0_RED, ps_textures)) {
        ++skipped_draws_;
        return;
    }

    VkFormat formats[8]{};
    for (uint32_t i = 0; i < color_count; ++i) formats[i] = colors[i] ? colors[i]->image.format : VK_FORMAT_UNDEFINED;
    const VkFormat depth_format = depth ? depth->image.format : VK_FORMAT_UNDEFINED;
    if (vs->module == VK_NULL_HANDLE || ps->module == VK_NULL_HANDLE) {
        ++deferred_draws_; // a shader still compiling
        return;
    }
    const Pipeline* pipe = pipeline(regs, vs, ps, formats, color_count, depth_format, topology_class);
    if (pipe == nullptr) {
        report_once("draw skipped: pipeline could not be created", 0);
        ++skipped_draws_;
        return;
    }
    if (pipe->state.load() != Built::kReady) {
        ++deferred_draws_; // its pipeline still compiling
        return;
    }

    // Per-draw constants.
    abi::DrawConstants dc{};
    const bool record = frames_per_frame_ > 1 && !replaying_;
    ConstantSources sources;
    for (uint32_t i = 0; i < 16; ++i) {
        if (const uint32_t a = r(reg::SQ_ALU_CONST_CACHE_VS_0 + i * 4)) {
            const uint32_t size = r(reg::SQ_ALU_CONST_BUFFER_SIZE_VS_0 + i * 4) << 8;
            dc.vs_cb[i] = buffer_descriptor(a << 8, size, 16);
            if (record && (vs->info.constant_bank_mask >> i) & 1) {
                sources.add({0, 0, static_cast<uint8_t>(i), a << 8, read_extent(size, vs->info.constant_bank_extent[i]), 16});
            }
        }
        if (const uint32_t a = r(reg::SQ_ALU_CONST_CACHE_PS_0 + i * 4)) {
            const uint32_t size = r(reg::SQ_ALU_CONST_BUFFER_SIZE_PS_0 + i * 4) << 8;
            dc.ps_cb[i] = buffer_descriptor(a << 8, size, 16);
            if (record && (ps->info.constant_bank_mask >> i) & 1) {
                sources.add({1, 0, static_cast<uint8_t>(i), a << 8, read_extent(size, ps->info.constant_bank_extent[i]), 16});
            }
        }
        const auto buffer = [&](uint32_t slot, int stage, uint32_t mask) {
            const uint32_t base = kResourceBase + slot * resource::kWords * 4;
            if ((r(base + 24) >> 30) != (resource::kTypeValidBuffer >> 30)) return abi::Buffer{};
            const uint32_t stride = (r(base + 8) >> 8) & 0x7FF;
            if (record && stage >= 0 && (mask >> i) & 1) {
                sources.add({static_cast<uint8_t>(stage), 1, static_cast<uint8_t>(i), r(base),
                             read_extent(r(base + 4) + 1, UINT32_MAX), stride});
            }
            return buffer_descriptor(r(base), r(base + 4) + 1, stride);
        };
        dc.vs_buf[i] = buffer(resource::kVsBuffer + i, 0, vs->info.buffer_mask);
        dc.ps_buf[i] = buffer(resource::kPsBuffer + i, 1, ps->info.buffer_mask);
        dc.vb[i] = buffer(resource::kVsAttrib + i, -1, 0);
    }
    // Stream-out writes guest memory, which a replay must not touch (the
    // draws that read it use what the frame wrote): a replayed draw keeps
    // its rasterised output, its stream-out buffers are empty.
    if (stream_out && !replaying_) {
        for (uint32_t i = 0; i < 4; ++i) {
            const uint32_t base = r(reg::VGT_STRMOUT_BUFFER_BASE_0 + i * 16) << 8;
            const uint32_t offset = r(reg::VGT_STRMOUT_BUFFER_OFFSET_0 + i * 16) * 4;
            const uint32_t size = r(reg::VGT_STRMOUT_BUFFER_SIZE_0 + i * 16) * 4;
            if (base != 0 && size > offset) dc.so[i] = buffer_descriptor(base + offset, size - offset, 0);
        }
    }
    dc.base_vertex = r(reg::SQ_VTX_BASE_VTX_LOC);
    dc.start_instance = r(reg::SQ_VTX_START_INST_LOC);
    dc.step_rate[0] = r(reg::VGT_INSTANCE_STEP_RATE_0);
    dc.step_rate[1] = r(reg::VGT_INSTANCE_STEP_RATE_0 + 4);
    // The pass's scale: its attachments are all upscaled or none is.
    float pass_scale = 0;
    bool mixed_scales = false;
    const auto take_scale = [&](const Target* t) {
        if (t == nullptr) return;
        if (pass_scale != 0 && pass_scale != t->scale) mixed_scales = true;
        pass_scale = std::max(pass_scale, t->scale);
    };
    for (uint32_t i = 0; i < color_count; ++i) take_scale(colors[i]);
    take_scale(depth);
    if (pass_scale == 0) pass_scale = 1;
    if (mixed_scales) {
        report_once("draw into upscaled and title-sized targets at once, twice the scale",
                    static_cast<uint32_t>(2 * pass_scale));
        for (uint32_t i = 0; i < color_count; ++i) {
            if (colors[i]) report_once("  mixed-scale pass: color target", colors[i]->pitch << 16 | colors[i]->height << 4 | static_cast<uint32_t>(colors[i]->scale));
        }
        if (depth) report_once("  mixed-scale pass: depth target", depth->pitch << 16 | depth->height << 4 | static_cast<uint32_t>(depth->scale));
    }
    dc.alpha_ref = std::bit_cast<float>(r(reg::SX_ALPHA_REF));
    dc.point_size = static_cast<float>((r(reg::PA_SU_POINT_SIZE) >> 16) & 0xFFFF) / 8.0f * pass_scale;
    dc.pixel_scale = pass_scale;
    dc.texture_scale = target_scale();
    for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
        if (vs_textures[slot].scaled) dc.vs_scaled_textures |= 1u << slot;
        if (ps_textures[slot].scaled) dc.ps_scaled_textures |= 1u << slot;
    }
    if (record) record_constants(draw_index, sources, dc);
    if (replaying_) replace_constants(draw_index, dc);
    const VkDeviceSize ubo_align = std::max<VkDeviceSize>(ctx_.properties.limits.minUniformBufferOffsetAlignment, 16);
    VkDeviceSize dc_offset = 0;
    std::memcpy(upload(sizeof(dc), ubo_align, dc_offset), &dc, sizeof(dc));
    VkDeviceSize reg_offset[2] = {};
    if (vs->info.uses_registers) {
        std::memcpy(upload(4096, ubo_align, reg_offset[0]), &regs.value[reg::SQ_ALU_CONSTANT0_256 >> 2], 4096);
    }
    if (ps->info.uses_registers) {
        std::memcpy(upload(4096, ubo_align, reg_offset[1]), &regs.value[reg::SQ_ALU_CONSTANT0_0 >> 2], 4096);
    }

    // Indices, converted to 32-bit host order (and to triangles for quads).
    bool indexed = d.source != Draw::kAuto || convert != Convert::kNone;
    uint32_t index_count = d.count;
    VkDeviceSize index_offset = 0;
    bool restart = false;
    if (indexed) {
        const uint32_t type = r(reg::VGT_DMA_INDEX_TYPE);
        const bool wide = type & 1;
        const uint32_t swap = (type >> 2) & 3;
        restart = (r(reg::VGT_MULTI_PRIM_IB_RESET_EN) & 1) && d.source != Draw::kAuto;
        const uint32_t restart_index = r(reg::VGT_MULTI_PRIM_IB_RESET_INDX);
        const uint8_t* source = nullptr;
        if (d.source != Draw::kAuto) {
            const uint32_t bytes = d.count * (wide ? 4 : 2);
            if (d.host_indices) {
                source = d.host_indices;
            } else {
                if (!guest_memory_committed(d.index_address, bytes)) {
                    report_once("draw skipped: index buffer outside guest memory, at", d.index_address);
                    ++skipped_draws_;
                    return;
                }
                source = guest_pointer(d.index_address);
            }
        }
        const auto index = [&](uint32_t i) -> uint32_t {
            if (source == nullptr) return i;
            uint32_t v;
            if (d.source == Draw::kImmediate) {
                // Packet dwords are big-endian; 16-bit indices low half first.
                const uint32_t word = __builtin_bswap32(reinterpret_cast<const uint32_t*>(source)[wide ? i : i / 2]);
                v = wide ? word : (i & 1 ? word >> 16 : word & 0xFFFF);
            } else if (wide) {
                std::memcpy(&v, source + i * 4, 4);
                if (swap == 2) v = __builtin_bswap32(v);
                else if (swap == 1) v = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);
            } else {
                uint16_t h;
                std::memcpy(&h, source + i * 2, 2);
                if (swap != 0) h = static_cast<uint16_t>((h >> 8) | (h << 8));
                v = h;
            }
            if (restart && v == restart_index) return 0xFFFFFFFFu;
            return v;
        };
        if (convert == Convert::kQuads) index_count = d.count / 4 * 6;
        else if (convert == Convert::kQuadStrip) index_count = d.count >= 4 ? (d.count - 2) / 2 * 6 : 0;
        else if (convert == Convert::kLoop) index_count = d.count + 1;
        if (index_count == 0) return;
        uint32_t* out = reinterpret_cast<uint32_t*>(upload(VkDeviceSize{index_count} * 4, 4, index_offset));
        switch (convert) {
        case Convert::kNone:
            if (d.source == Draw::kIndexBuffer && (wide ? swap == 2 || swap == 0 : true)) {
                // The common case, in loops the compiler vectorises.
                const uint32_t marker = restart ? restart_index : 0xFFFFFFFFu;
                if (wide) {
                    const uint32_t* in = reinterpret_cast<const uint32_t*>(source);
                    for (uint32_t i = 0; i < index_count; ++i) {
                        uint32_t v;
                        std::memcpy(&v, in + i, 4);
                        v = swap == 2 ? __builtin_bswap32(v) : v;
                        out[i] = v == marker ? 0xFFFFFFFFu : v;
                    }
                } else {
                    const uint16_t* in = reinterpret_cast<const uint16_t*>(source);
                    for (uint32_t i = 0; i < index_count; ++i) {
                        uint16_t h;
                        std::memcpy(&h, in + i, 2);
                        const uint32_t v = swap != 0 ? static_cast<uint16_t>((h >> 8) | (h << 8)) : h;
                        out[i] = v == marker ? 0xFFFFFFFFu : v;
                    }
                }
                break;
            }
            for (uint32_t i = 0; i < index_count; ++i) out[i] = index(i);
            break;
        case Convert::kQuads:
            for (uint32_t q = 0; q < d.count / 4; ++q) {
                static constexpr uint32_t kOrder[6] = {0, 1, 2, 0, 2, 3};
                for (int k = 0; k < 6; ++k) out[q * 6 + k] = index(q * 4 + kOrder[k]);
            }
            break;
        case Convert::kQuadStrip:
            for (uint32_t q = 0; q < (d.count - 2) / 2; ++q) {
                static constexpr uint32_t kOrder[6] = {0, 1, 3, 0, 3, 2};
                for (int k = 0; k < 6; ++k) out[q * 6 + k] = index(q * 2 + kOrder[k]);
            }
            break;
        case Convert::kLoop:
            for (uint32_t i = 0; i < d.count; ++i) out[i] = index(i);
            out[d.count] = index(0);
            break;
        }
    }

    // Stream-out data a run of stream-out draws wrote, read by the draws
    // after it (skinned vertices, text): a barrier first, outside the pass.
    const bool writes_stream_out = stream_out && !replaying_;
    if (stream_out_pending_ && !writes_stream_out) {
        if (rendering_) end_rendering();
        else barrier();
    }

    // The rendering pass.
    if (!rendering_ || pass_depth_ != depth ||
        !std::equal(colors.begin(), colors.end(), pass_colors_.begin())) {
        end_rendering();
        uint32_t width = UINT32_MAX, height = UINT32_MAX;
        VkRenderingAttachmentInfo ca[8]{};
        for (uint32_t i = 0; i < color_count; ++i) {
            ca[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            if (colors[i] == nullptr) continue;
            ca[i].imageView = colors[i]->image.view;
            ca[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            ca[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            width = std::min(width, colors[i]->image.width);
            height = std::min(height, colors[i]->image.height);
        }
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (depth) {
            da.imageView = depth->image.view;
            da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            width = std::min(width, depth->image.width);
            height = std::min(height, depth->image.height);
        }
        if (width == UINT32_MAX) width = height = 1;
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {width, height}};
        info.layerCount = 1;
        info.colorAttachmentCount = color_count;
        info.pColorAttachments = ca;
        info.pDepthAttachment = depth ? &da : nullptr;
        info.pStencilAttachment = depth && has_stencil(depth->image.format) ? &da : nullptr;
        vkCmdBeginRendering(cmd(), &info);
        rendering_ = true;
        pass_colors_ = colors;
        pass_depth_ = depth;
        pass_width_ = width;
        pass_height_ = height;
    }
    VkCommandBuffer c = cmd();
    if (bound_pipeline_ != pipe) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
        bound_pipeline_ = pipe;
    }

    // Descriptors.
    std::vector<VkWriteDescriptorSet> writes;
    VkDescriptorBufferInfo buffers[3];
    VkDescriptorImageInfo images[2 * abi::kTextureSlots];
    writes.reserve(3 + 2 * abi::kTextureSlots);
    const auto write_buffer = [&](uint32_t binding, VkDeviceSize offset, VkDeviceSize range) {
        VkDescriptorBufferInfo& b = buffers[writes.size()];
        b = {ring_.buffer(), offset, range};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w.pBufferInfo = &b;
        writes.push_back(w);
    };
    write_buffer(abi::kDrawConstantsBinding, dc_offset, sizeof(dc));
    if (vs->info.uses_registers) write_buffer(abi::kVsRegistersBinding, reg_offset[0], 4096);
    if (ps->info.uses_registers) write_buffer(abi::kPsRegistersBinding, reg_offset[1], 4096);
    uint32_t image_count = 0;
    const auto write_textures = [&](const ShaderModule* s, const Bound* bound, uint32_t binding_base) {
        for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
            if (!(s->info.texture_mask & (1u << slot))) continue;
            VkDescriptorImageInfo& ii = images[image_count++];
            ii = {bound[slot].sampler, bound[slot].view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = binding_base + slot;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &ii;
            writes.push_back(w);
        }
    };
    write_textures(vs, vs_textures, abi::kVsTextureBinding);
    write_textures(ps, ps_textures, abi::kPsTextureBinding);
    ctx_.cmd_push_descriptor_set(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->layout, 0,
                                 static_cast<uint32_t>(writes.size()), writes.data());

    // Viewport: the Latte transform as is (negative heights flip y).
    const float xscale = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0));
    const float xoffset = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 4));
    const float yscale = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 8));
    const float yoffset = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 12));
    const float zscale = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 16));
    const float zoffset = std::bit_cast<float>(r(reg::PA_CL_VPORT_XSCALE_0 + 20));
    VkViewport viewport{};
    const float vscale = pass_scale;
    viewport.x = (xoffset - xscale) * vscale;
    viewport.width = 2.0f * xscale * vscale;
    viewport.y = (yoffset - yscale) * vscale;
    viewport.height = 2.0f * yscale * vscale;
    if (viewport.width <= 0.0f) {
        viewport.x += viewport.width;
        viewport.width = std::max(-viewport.width, 1.0f);
    }
    if (vs_env.clip_space_dx) {
        viewport.minDepth = zoffset;
        viewport.maxDepth = zoffset + zscale;
    } else {
        viewport.minDepth = zoffset - zscale;
        viewport.maxDepth = zoffset + zscale;
    }
    viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
    viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    if ((r(reg::PA_CL_VTE_CNTL) & 0x3F) != 0x3F) report_once("viewport transform partly disabled", r(reg::PA_CL_VTE_CNTL));
    vkCmdSetViewport(c, 0, 1, &viewport);
    const uint32_t tl = r(reg::PA_SC_GENERIC_SCISSOR_TL), br = r(reg::PA_SC_GENERIC_SCISSOR_TL + 4);
    const uint32_t vtl = r(reg::PA_SC_VPORT_SCISSOR_0_TL), vbr = r(reg::PA_SC_VPORT_SCISSOR_0_TL + 4);
    int32_t x0 = std::max<int32_t>(tl & 0x7FFF, vtl & 0x7FFF), y0 = std::max<int32_t>((tl >> 16) & 0x7FFF, (vtl >> 16) & 0x7FFF);
    int32_t x1 = std::min<int32_t>(br & 0x7FFF, vbr & 0x7FFF), y1 = std::min<int32_t>((br >> 16) & 0x7FFF, (vbr >> 16) & 0x7FFF);
    // An edge takes the image pixels whose centres are inside it: e * scale
    // at the whole scales; at 1.5 the pixels a title pixel covers more than
    // half of (as the rasteriser's coverage of the scaled viewport).
    const auto edge = [&](int32_t e) { return static_cast<int32_t>(std::ceil(e * pass_scale - 0.5f)); };
    x0 = edge(x0);
    y0 = edge(y0);
    x1 = edge(x1);
    y1 = edge(y1);
    x1 = std::min<int32_t>(x1, static_cast<int32_t>(pass_width_));
    y1 = std::min<int32_t>(y1, static_cast<int32_t>(pass_height_));
    VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(std::max(x1 - x0, 0)), static_cast<uint32_t>(std::max(y1 - y0, 0))}};
    vkCmdSetScissor(c, 0, 1, &scissor);

    const uint32_t mode = r(reg::PA_SU_SC_MODE_CNTL);
    VkCullModeFlags cull = 0;
    if (mode & 1) cull |= VK_CULL_MODE_FRONT_BIT;
    if (mode & 2) cull |= VK_CULL_MODE_BACK_BIT;
    vkCmdSetCullMode(c, cull);
    // FACE: 0 counter-clockwise front, in the y-down window space Vulkan uses too.
    vkCmdSetFrontFace(c, (mode >> 2) & 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(c, topology);
    vkCmdSetPrimitiveRestartEnable(c, restart && topology_class != 0 && topology != VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST &&
                                          topology != VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
    vkCmdSetRasterizerDiscardEnable(c, rasterizer_discard);
    vkCmdSetLineWidth(c, 1.0f);
    const bool offset_enable = (mode >> 11) & 1;
    vkCmdSetDepthBiasEnable(c, offset_enable);
    if (offset_enable) {
        const float scale = std::bit_cast<float>(r(reg::PA_SU_POLY_OFFSET_FRONT_SCALE));
        const float offset = std::bit_cast<float>(r(reg::PA_SU_POLY_OFFSET_FRONT_SCALE + 4));
        const float clamp = std::bit_cast<float>(r(reg::PA_SU_POLY_OFFSET_CLAMP));
        vkCmdSetDepthBias(c, offset, clamp, scale / 16.0f);
    }
    float blend_constants[4];
    for (int i = 0; i < 4; ++i) blend_constants[i] = std::bit_cast<float>(r(reg::CB_BLEND_RED + i * 4));
    vkCmdSetBlendConstants(c, blend_constants);

    const bool has_depth = depth != nullptr;
    const bool has_stencil_aspect = has_depth && (depth->image.aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    // TTT2_TRACE_NO_DEPTH=first,last: those draws of the traced frame (counted from 1) without the
    // depth test, to find which draw a surface belongs to.
    bool no_depth = false;
    if (tracing()) {
        static uint64_t frame = UINT64_MAX;
        static uint32_t count = 0;
        if (frame != frame_number_) frame = frame_number_, count = 0;
        ++count;
        static const char* range = std::getenv("TTT2_TRACE_NO_DEPTH");
        no_depth = range && count >= std::strtoul(range, nullptr, 10) &&
                   count <= std::strtoul(std::strchr(range, ',') + 1, nullptr, 10);
    }
    vkCmdSetDepthTestEnable(c, has_depth && ((depth_control >> 1) & 1));
    vkCmdSetDepthWriteEnable(c, has_depth && (((depth_control >> 2) & 1) || no_depth)); // (no_depth: shows its depth)
    // (no_depth: always passes, still writing depth.)
    vkCmdSetDepthCompareOp(c, no_depth ? VK_COMPARE_OP_ALWAYS : static_cast<VkCompareOp>((depth_control >> 4) & 7));
    vkCmdSetDepthBoundsTestEnable(c, VK_FALSE);
    const bool stencil = has_stencil_aspect && (depth_control & 1);
    vkCmdSetStencilTestEnable(c, stencil);
    // Separate back-face stencil only for draws that cull: Smash's menu emblem
    // draws unculled quads that come out back-facing here with a back
    // function of NEVER, yet shows them on the console.
    // shortcut: unverified on hardware, recheck if a title's two-sided stencil (shadow volumes) breaks.
    const bool backface = (depth_control >> 7) & 1 && (mode & 3) != 0;
    const auto op = [](uint32_t v) { return static_cast<VkStencilOp>(v & 7); };
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, op(depth_control >> 11), op(depth_control >> 14), op(depth_control >> 17),
                      static_cast<VkCompareOp>((depth_control >> 8) & 7));
    if (backface) {
        vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, op(depth_control >> 23), op(depth_control >> 26),
                          op(depth_control >> 29), static_cast<VkCompareOp>((depth_control >> 20) & 7));
    } else {
        vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, op(depth_control >> 11), op(depth_control >> 14),
                          op(depth_control >> 17), static_cast<VkCompareOp>((depth_control >> 8) & 7));
    }
    const uint32_t ref = r(reg::DB_STENCILREFMASK), ref_bf = backface ? r(reg::DB_STENCILREFMASK_BF) : ref;
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, ref & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, (ref >> 8) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, (ref >> 16) & 0xFF);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, ref_bf & 0xFF);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, (ref_bf >> 8) & 0xFF);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, (ref_bf >> 16) & 0xFF);

    if (tracing()) {
        uint64_t vh = 0, ph = 0, fh = 0;
        program(r(reg::SQ_PGM_START_VS) << 8, r(reg::SQ_PGM_START_VS + 4) << 3, vh);
        program(r(reg::SQ_PGM_START_PS) << 8, r(reg::SQ_PGM_START_PS + 4) << 3, ph);
        program(r(reg::SQ_PGM_START_FS) << 8, r(reg::SQ_PGM_START_FS + 4) << 3, fh);
        std::fprintf(stderr, "trace: draw prim 0x%X count %u%s inst %u vs %016llx ps %016llx fs %016llx\n",
                     r(reg::VGT_PRIMITIVE_TYPE), d.count, indexed ? " indexed" : "", instances,
                     static_cast<unsigned long long>(vh), static_cast<unsigned long long>(ph),
                     static_cast<unsigned long long>(fh));
        std::fprintf(stderr, "trace:   spi_ps_in 0x%08X 0x%08X interp 0x%08X vs_out_cntl 0x%08X point 0x%08X\n",
                     r(reg::SPI_PS_IN_CONTROL_0), r(reg::SPI_PS_IN_CONTROL_0 + 4), r(reg::SPI_INTERP_CONTROL_0),
                     r(reg::PA_CL_VS_OUT_CNTL), r(reg::PA_SU_POINT_SIZE));
        if (color_count == 0) {
            std::fprintf(stderr, "trace:   no color target: mask 0x%08X cb0 base 0x%08X size 0x%08X info 0x%08X control 0x%08X\n",
                         target_mask, r(reg::CB_COLOR0_BASE), r(reg::CB_COLOR0_SIZE), r(reg::CB_COLOR0_INFO),
                         r(reg::CB_COLOR_CONTROL));
        }
        for (uint32_t i = 0; i < color_count; ++i) {
            if (!colors[i]) continue;
            std::fprintf(stderr, "trace:   color%u 0x%08X fmt 0x%03X %ux%u tile %u mask 0x%X blend %s 0x%08X\n", i,
                         colors[i]->address, colors[i]->format, colors[i]->pitch, colors[i]->height,
                         colors[i]->tile_mode, (target_mask >> (4 * i)) & 0xF,
                         (r(reg::CB_COLOR_CONTROL) >> (8 + i)) & 1 ? "on" : "off", r(reg::CB_BLEND0_CONTROL + i * 4));
        }
        if (depth) {
            std::fprintf(stderr, "trace:   depth 0x%08X fmt 0x%03X %ux%u control 0x%08X stencil ref 0x%08X bf 0x%08X mode 0x%X\n",
                         depth->address, depth->format, depth->pitch, depth->height, depth_control,
                         r(reg::DB_STENCILREFMASK), r(reg::DB_STENCILREFMASK_BF), mode);
        }
        const auto trace_textures = [&](const ShaderModule* s, uint32_t base, const char* stage) {
            for (uint32_t slot = 0; slot < abi::kTextureSlots; ++slot) {
                if (!(s->info.texture_mask & (1u << slot))) continue;
                uint32_t w[7];
                for (int i = 0; i < 7; ++i) w[i] = r(kResourceBase + ((base + slot) * resource::kWords + i) * 4);
                const Texture* t = texture(w);
                std::fprintf(stderr, "trace:   %s tex%u 0x%08X fmt 0x%03X %ux%ux%u dim %u tile %u pitch %u levels %u sel %03o %s\n",
                             stage, slot, w[2] << 8, texture_surface_format(w[1], w[4]), ((w[0] >> 19) & 0x1FFF) + 1,
                             (w[1] & 0x1FFF) + 1, ((w[1] >> 13) & 0x1FFF) + 1, w[0] & 7, (w[0] >> 3) & 0xF,
                             ((w[0] >> 8) & 0x7FF) * 8 + 8, (w[5] & 0xF) + 1, (w[4] >> 16) & 0xFFF,
                             t && t->source ? "(render target)" : "(memory)");
            }
        };
        trace_textures(vs, resource::kVsTexture, "vs");
        trace_textures(ps, resource::kPsTexture, "ps");
        std::fprintf(stderr, "trace:   viewport %g,%g %gx%g depth %g-%g scissor %d,%d %ux%u cull 0x%X\n", viewport.x,
                     viewport.y, viewport.width, viewport.height, viewport.minDepth, viewport.maxDepth, scissor.offset.x,
                     scissor.offset.y, scissor.extent.width, scissor.extent.height, mode & 7);
    }
    if (indexed) {
        vkCmdBindIndexBuffer(c, ring_.buffer(), index_offset, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(c, index_count, instances, 0, 0, 0);
    } else {
        vkCmdDraw(c, d.count, instances, 0, 0);
    }
    if (writes_stream_out) stream_out_pending_ = true;
    // TTT2_TRACE_DRAWS=first,last: the traced frame's color target after each of those draws (from 1), as PNGs.
    if (tracing()) {
        static uint64_t frame = UINT64_MAX;
        static uint32_t n = 0;
        if (frame != frame_number_) { frame = frame_number_; n = 0; }
        ++n;
        static const char* range = std::getenv("TTT2_TRACE_DRAWS");
        static const char* dir = std::getenv("TTT2_CAPTURE");
        if (colors[0] && range && dir && n >= std::strtoul(range, nullptr, 10) &&
            n <= std::strtoul(std::strchr(range, ',') + 1, nullptr, 10)) {
            char name[64];
            std::snprintf(name, sizeof(name), "/draw%03u.png", n);
            end_rendering();
            save_image(colors[0]->image, std::string(dir) + name);
        }
        // TTT2_TRACE_DIFF=first,last: for those draws, the box of depth texels each changed and the depth
        // values it wrote there (whole depth target read back after each).
        static const char* diff = std::getenv("TTT2_TRACE_DIFF");
        static std::map<const void*, std::vector<float>> before;
        if (diff && depth && n >= std::strtoul(diff, nullptr, 10) && n <= std::strtoul(std::strchr(diff, ',') + 1, nullptr, 10) &&
            (depth->image.format == VK_FORMAT_D32_SFLOAT_S8_UINT || depth->image.format == VK_FORMAT_D32_SFLOAT)) {
            const uint32_t w = depth->image.width, h = depth->image.height;
            VkDeviceSize zoff = 0;
            upload(VkDeviceSize{w} * h * 4, 16, zoff);
            end_rendering();
            barrier();
            VkBufferImageCopy zr{};
            zr.bufferOffset = zoff;
            zr.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
            zr.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd(), depth->image.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &zr);
            submit(true);
            std::vector<float> now(size_t{w} * h);
            std::memcpy(now.data(), ring_.mapped() + zoff, now.size() * 4);
            std::vector<float>& old = before[depth];
            if (old.size() == now.size()) {
                uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0, count = 0;
                float lo = 2, hi = -2;
                for (uint32_t y = 0; y < h; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        const float v = now[size_t{y} * w + x];
                        if (v == old[size_t{y} * w + x]) continue;
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y);
                        lo = std::min(lo, v), hi = std::max(hi, v), ++count;
                    }
                }
                if (count) {
                    std::fprintf(stderr, "diff: draw %u depth 0x%08X %ux%u: %u texels in (%u,%u)-(%u,%u), depth %.6f..%.6f\n", n,
                                 depth->address, w, h, count, x0, y0, x1, y1, lo, hi);
                }
            }
            old = std::move(now);
        }
        // (And the color target's changed texels, raw bytes compared.)
        static std::map<const void*, std::vector<uint8_t>> color_before;
        if (const Target* t = colors[0]; diff && t && n >= std::strtoul(diff, nullptr, 10) &&
                                         n <= std::strtoul(std::strchr(diff, ',') + 1, nullptr, 10)) {
            const uint32_t w = t->image.width, h = t->image.height, texel = 8;
            VkDeviceSize coff = 0;
            upload(VkDeviceSize{w} * h * texel, 16, coff);
            end_rendering();
            barrier();
            VkBufferImageCopy cr{};
            cr.bufferOffset = coff;
            cr.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cr.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &cr);
            submit(true);
            const uint8_t* p = ring_.mapped() + coff;
            std::vector<uint8_t> now(p, p + size_t{w} * h * texel);
            std::vector<uint8_t>& old = color_before[t];
            if (old.size() == now.size()) {
                uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0, count = 0;
                for (uint32_t y = 0; y < h; ++y) {
                    for (uint32_t x = 0; x < w; ++x) {
                        const size_t i = (size_t{y} * w + x) * texel;
                        if (std::memcmp(&now[i], &old[i], texel) == 0) continue;
                        x0 = std::min(x0, x), y0 = std::min(y0, y), x1 = std::max(x1, x), y1 = std::max(y1, y), ++count;
                    }
                }
                if (count) {
                    std::fprintf(stderr, "diff: draw %u color 0x%08X %ux%u: %u texels in (%u,%u)-(%u,%u)\n", n, t->address, w, h,
                                 count, x0, y0, x1, y1);
                }
            }
            old = std::move(now);
        }
        // TTT2_TRACE_PIXEL=x,y[,r] (title pixels): each draw of the traced frame that changes that pixel
        // (or the box of half-size r around it) of its color target, with a hash and the first texel.
        static const char* probe = std::getenv("TTT2_TRACE_PIXEL");
        static std::map<const void*, uint64_t> last;
        if (const Target* t = colors[0]; probe && t && t->image.width != 0) {
            const char* second = std::strchr(probe, ',') + 1;
            const char* third = std::strchr(second, ',');
            const uint32_t r = third ? std::strtoul(third + 1, nullptr, 10) : 0;
            // (Values below 1: fractions of the target's size, the same spot at any resolution.)
            const float fx = std::strtof(probe, nullptr), fy = std::strtof(second, nullptr);
            const int32_t cx = fx < 1 ? static_cast<int32_t>(fx * t->image.width)
                                      : static_cast<int32_t>(scaled(static_cast<uint32_t>(fx), t->scale));
            const int32_t cy = fy < 1 ? static_cast<int32_t>(fy * t->image.height)
                                      : static_cast<int32_t>(scaled(static_cast<uint32_t>(fy), t->scale));
            const int32_t sr = static_cast<int32_t>(scaled(r, t->scale));
            const int32_t x0 = std::max(0, cx - sr), y0 = std::max(0, cy - sr);
            const int32_t x1 = std::min<int32_t>(t->image.width, cx + sr + 1), y1 = std::min<int32_t>(t->image.height, cy + sr + 1);
            if (x0 < x1 && y0 < y1) {
                const uint32_t w = x1 - x0, h = y1 - y0, texel = 8, bytes = w * h * texel;
                VkDeviceSize offset = 0;
                upload(bytes, 16, offset);
                end_rendering();
                barrier();
                VkBufferImageCopy region{};
                region.bufferOffset = offset;
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageOffset = {x0, y0, 0};
                region.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer(cmd(), t->image.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &region);
                submit(true);
                const uint8_t* p = ring_.mapped() + offset;
                uint64_t hsh = 1469598103934665603ull; // FNV-1a
                for (uint32_t i = 0; i < bytes; ++i) hsh = (hsh ^ p[i]) * 1099511628211ull;
                if (last[t] != hsh) {
                    last[t] = hsh;
                    float z = -1;
                    if (depth && (depth->image.format == VK_FORMAT_D32_SFLOAT_S8_UINT || depth->image.format == VK_FORMAT_D32_SFLOAT) &&
                        static_cast<uint32_t>(cx) < depth->image.width && static_cast<uint32_t>(cy) < depth->image.height) {
                        VkDeviceSize zoff = 0;
                        upload(4, 16, zoff);
                        VkBufferImageCopy zr{};
                        zr.bufferOffset = zoff;
                        zr.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                        zr.imageOffset = {cx, cy, 0};
                        zr.imageExtent = {1, 1, 1};
                        vkCmdCopyImageToBuffer(cmd(), depth->image.image, VK_IMAGE_LAYOUT_GENERAL, ring_.buffer(), 1, &zr);
                        submit(true);
                        std::memcpy(&z, ring_.mapped() + zoff, 4);
                    }
                    std::fprintf(stderr, "probe: draw %u color 0x%08X fmt 0x%03X: hash %016llx first %02X%02X%02X%02X depth %.7f\n", n,
                                 t->address, t->format, static_cast<unsigned long long>(hsh), p[0], p[1], p[2], p[3], z);
                }
            }
        }
    }
    if (profiling_) {
        profile_mark("draw", colors[0] ? colors[0]->address : depth ? depth->address : 0,
                     (colors[0] ? scaled(colors[0]->pitch, colors[0]->scale) : 0) << 16 |
                         (colors[0] ? scaled(colors[0]->height, colors[0]->scale) : 0),
                     d.count * std::max(instances, 1u));
    }

    const uint64_t now = stamp();
    for (uint32_t i = 0; i < color_count; ++i) {
        if (colors[i]) {
            const_cast<Target*>(colors[i])->written = now;
            note_target_written(colors[i]);
        }
    }
    if (depth && ((depth_control >> 2) & 1 || stencil)) {
        const_cast<Target*>(depth)->written = now;
        note_target_written(depth);
    }
}

} // namespace cafe::gpu::vk
