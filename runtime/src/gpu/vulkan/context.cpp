#include "gpu/vulkan/context.h"

#include "cafe/layout.h"
#include "cafe/runtime.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace cafe::gpu::vk {

namespace {
// VK_EXT_device_fault, when the driver has it: what the GPU hit when the device is lost.
VkDevice g_fault_device = VK_NULL_HANDLE;
PFN_vkGetDeviceFaultInfoEXT g_get_fault_info = nullptr;

void report_device_fault() {
    if (!g_get_fault_info) {
        std::fprintf(stderr, "ttt2: Vulkan: device lost (the driver has no VK_EXT_device_fault to say why)\n");
        return;
    }
    VkDeviceFaultCountsEXT counts{VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT};
    if (g_get_fault_info(g_fault_device, &counts, nullptr) != VK_SUCCESS) return;
    std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
    std::vector<VkDeviceFaultVendorInfoEXT> vendor(counts.vendorInfoCount);
    VkDeviceFaultInfoEXT info{VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT};
    info.pAddressInfos = addresses.data();
    info.pVendorInfos = vendor.data();
    counts.vendorBinarySize = 0;
    if (g_get_fault_info(g_fault_device, &counts, &info) != VK_SUCCESS && addresses.empty()) return;
    std::fprintf(stderr, "ttt2: Vulkan: device fault: %s\n", info.description);
    static const char* const kTypes[] = {"none", "invalid read", "invalid write", "invalid execute",
                                         "instruction pointer unknown", "instruction pointer invalid",
                                         "instruction pointer fault"};
    for (const auto& a : addresses) {
        std::fprintf(stderr, "ttt2: Vulkan:   %s at 0x%016llx (+/- 0x%llx)\n",
                     a.addressType < std::size(kTypes) ? kTypes[a.addressType] : "?",
                     static_cast<unsigned long long>(a.reportedAddress), static_cast<unsigned long long>(a.addressPrecision));
    }
    for (const auto& v : vendor) {
        std::fprintf(stderr, "ttt2: Vulkan:   vendor: %s (code 0x%llx, data 0x%llx)\n", v.description,
                     static_cast<unsigned long long>(v.vendorFaultCode), static_cast<unsigned long long>(v.vendorFaultData));
    }
}
} // namespace

void check_failed(const char* expression, VkResult result, const char* file, int line) {
    if (result == VK_ERROR_DEVICE_LOST) report_device_fault();
    fatal("Vulkan: %s failed with %d (%s:%d)", expression, static_cast<int>(result), file, line);
}

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& list, const char* name) {
    for (const auto& e : list) {
        if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_callback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                              VkDebugUtilsMessageTypeFlagsEXT,
                                              const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        std::fprintf(stderr, "ttt2: vulkan: %s\n", data->pMessage);
    }
    return VK_FALSE;
}

} // namespace

void Context::init(const std::vector<const char*>& instance_extensions) {
    std::vector<const char*> extensions = instance_extensions;
    std::vector<const char*> layers;
    // TTT2_VK_VALIDATION=1 enables the Khronos validation layer if installed.
    if (const char* v = std::getenv("TTT2_VK_VALIDATION"); v && *v == '1') {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        for (const auto& l : available) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) validation = true;
        }
        if (validation) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            std::fprintf(stderr, "ttt2: TTT2_VK_VALIDATION: the validation layer is not installed\n");
        }
    }
    {
        uint32_t count = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
        surface_capabilities2 = has_extension(available, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        if (surface_capabilities2 &&
            std::none_of(extensions.begin(), extensions.end(), [](const char* e) {
                return std::strcmp(e, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0;
            })) {
            extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
        }
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Tekken Tag Tournament 2";
    app.pEngineName = "ttt2";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    VK_CHECK(vkCreateInstance(&ici, nullptr, &instance));
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT dci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        dci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        dci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        dci.pfnUserCallback = debug_callback;
        VkDebugUtilsMessengerEXT messenger;
        if (create) create(instance, &dci, nullptr, &messenger);
    }

    // A discrete GPU with everything the renderer needs, else the first that has it.
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    static const char* const kRequired[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
                                            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    int best = -1;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t n = 0;
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> exts(n);
        vkEnumerateDeviceExtensionProperties(devices[i], nullptr, &n, exts.data());
        bool ok = true;
        for (const char* r : kRequired) ok &= has_extension(exts, r);
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devices[i], &p);
        if (!ok || p.apiVersion < VK_API_VERSION_1_3) continue;
        const bool discrete = p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        if (best < 0 || (discrete && properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) {
            best = static_cast<int>(i);
            properties = p;
        }
    }
    if (best < 0) {
        fatal("Vulkan: no GPU supports Vulkan 1.3 with swapchain, external host memory and push descriptors");
    }
    physical = devices[best];
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    std::fprintf(stderr, "ttt2: GPU: %s\n", properties.deviceName);

    uint32_t family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
    std::vector<VkQueueFamilyProperties> families(family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
            queue_family = i;
            break;
        }
    }
    if (queue_family == UINT32_MAX) fatal("Vulkan: no graphics queue");
    for (uint32_t i = 0; i < family_count; ++i) {
        if ((families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && !(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            present_family = i;
            break;
        }
    }

    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> available(n);
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, available.data());
    std::vector<const char*> device_extensions(std::begin(kRequired), std::end(kRequired));
    custom_border_color = has_extension(available, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    if (custom_border_color) device_extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    present_wait = surface_capabilities2 && has_extension(available, VK_KHR_PRESENT_ID_2_EXTENSION_NAME) &&
                   has_extension(available, VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);

    VkPhysicalDeviceCustomBorderColorFeaturesEXT border{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f12;
    f12.pNext = &f13;
    VkPhysicalDevicePresentId2FeaturesKHR id2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentWait2FeaturesKHR wait2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    id2.pNext = &wait2;
    border.pNext = present_wait ? &id2 : nullptr;
    f13.pNext = custom_border_color ? static_cast<void*>(&border) : present_wait ? static_cast<void*>(&id2) : nullptr;
    vkGetPhysicalDeviceFeatures2(physical, &f2);
    present_wait = present_wait && id2.presentId2 && wait2.presentWait2;
    const auto require = [](VkBool32 feature, const char* name) {
        if (!feature) fatal("Vulkan: the GPU lacks %s", name);
    };
    require(f12.bufferDeviceAddress, "bufferDeviceAddress");
    require(f13.dynamicRendering, "dynamicRendering");
    require(f13.synchronization2, "synchronization2");
    require(f13.shaderDemoteToHelperInvocation, "shaderDemoteToHelperInvocation");
    require(f2.features.vertexPipelineStoresAndAtomics, "vertexPipelineStoresAndAtomics");
    require(f2.features.textureCompressionBC, "textureCompressionBC");

    VkPhysicalDeviceVulkan13Features e13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    e13.dynamicRendering = VK_TRUE;
    e13.synchronization2 = VK_TRUE;
    e13.shaderDemoteToHelperInvocation = VK_TRUE;
    e13.maintenance4 = f13.maintenance4;
    VkPhysicalDeviceVulkan12Features e12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    e12.bufferDeviceAddress = VK_TRUE;
    e12.samplerMirrorClampToEdge = f12.samplerMirrorClampToEdge;
    e12.timelineSemaphore = f12.timelineSemaphore;
    e12.pNext = &e13;
    VkPhysicalDeviceCustomBorderColorFeaturesEXT eborder{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    if (custom_border_color) {
        eborder.customBorderColors = border.customBorderColors;
        eborder.customBorderColorWithoutFormat = border.customBorderColorWithoutFormat;
        custom_border_color = border.customBorderColors && border.customBorderColorWithoutFormat;
        e13.pNext = &eborder;
    }
    VkPhysicalDevicePresentId2FeaturesKHR eid2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_2_FEATURES_KHR};
    VkPhysicalDevicePresentWait2FeaturesKHR ewait2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_2_FEATURES_KHR};
    if (present_wait) {
        device_extensions.push_back(VK_KHR_PRESENT_ID_2_EXTENSION_NAME);
        device_extensions.push_back(VK_KHR_PRESENT_WAIT_2_EXTENSION_NAME);
        eid2.presentId2 = VK_TRUE;
        ewait2.presentWait2 = VK_TRUE;
        eid2.pNext = &ewait2;
        ewait2.pNext = const_cast<void*>(e13.pNext);
        e13.pNext = &eid2;
    }
    VkPhysicalDeviceFeatures2 e2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    e2.pNext = &e12;
    // A report when the GPU faults (the device is lost), if the driver can give one.
    VkPhysicalDeviceFaultFeaturesEXT fault{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
    const bool device_fault = has_extension(available, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    if (device_fault) {
        device_extensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        fault.deviceFault = VK_TRUE;
        fault.pNext = e2.pNext;
        e2.pNext = &fault;
    }
    VkPhysicalDeviceFeatures& ef = e2.features;
    const VkPhysicalDeviceFeatures& af = f2.features;
    ef.vertexPipelineStoresAndAtomics = VK_TRUE;
    ef.textureCompressionBC = VK_TRUE;
    ef.independentBlend = af.independentBlend;
    ef.dualSrcBlend = af.dualSrcBlend;
    ef.logicOp = af.logicOp;
    ef.depthClamp = af.depthClamp;
    ef.depthBiasClamp = af.depthBiasClamp;
    ef.fillModeNonSolid = af.fillModeNonSolid;
    ef.wideLines = af.wideLines;
    ef.largePoints = af.largePoints;
    ef.samplerAnisotropy = af.samplerAnisotropy;
    ef.shaderClipDistance = af.shaderClipDistance;
    ef.imageCubeArray = af.imageCubeArray;
    ef.fragmentStoresAndAtomics = af.fragmentStoresAndAtomics;
    ef.shaderImageGatherExtended = af.shaderImageGatherExtended;
    ef.depthBounds = af.depthBounds;
    ef.shaderStorageImageWriteWithoutFormat = af.shaderStorageImageWriteWithoutFormat;
    storage_without_format = af.shaderStorageImageWriteWithoutFormat;

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci[2]{};
    qci[0].sType = qci[1].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci[0].queueFamilyIndex = queue_family;
    qci[0].queueCount = 1;
    qci[0].pQueuePriorities = &priority;
    qci[1].queueFamilyIndex = present_family;
    qci[1].queueCount = 1;
    qci[1].pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &e2;
    dci.queueCreateInfoCount = present_family != UINT32_MAX ? 2 : 1;
    dci.pQueueCreateInfos = qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(device_extensions.size());
    dci.ppEnabledExtensionNames = device_extensions.data();
    VK_CHECK(vkCreateDevice(physical, &dci, nullptr, &device));
    if (device_fault) {
        g_fault_device = device;
        g_get_fault_info = reinterpret_cast<PFN_vkGetDeviceFaultInfoEXT>(vkGetDeviceProcAddr(device, "vkGetDeviceFaultInfoEXT"));
    }
    std::fprintf(stderr, "ttt2: GPU: fault reports %s\n", g_get_fault_info ? "on" : "not available");
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    if (present_family != UINT32_MAX) vkGetDeviceQueue(device, present_family, 0, &present_queue);
    get_memory_host_pointer_properties = reinterpret_cast<PFN_vkGetMemoryHostPointerPropertiesEXT>(
        vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT"));
    cmd_push_descriptor_set =
        reinterpret_cast<PFN_vkCmdPushDescriptorSetKHR>(vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR"));
    if (present_wait) {
        wait_for_present = reinterpret_cast<PFN_vkWaitForPresent2KHR>(vkGetDeviceProcAddr(device, "vkWaitForPresent2KHR"));
        present_wait = wait_for_present != nullptr;
    }
}

uint32_t Context::memory_type(uint32_t type_bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    fatal("Vulkan: no memory type 0x%X with properties 0x%X", type_bits, flags);
}

Buffer Context::create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags flags) {
    Buffer b;
    b.size = size;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VK_CHECK(vkCreateBuffer(device, &bci, nullptr, &b.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateFlagsInfo flags_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags_info.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &flags_info;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, flags);
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &b.memory));
    VK_CHECK(vkBindBufferMemory(device, b.buffer, b.memory, 0));
    if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        void* p = nullptr;
        VK_CHECK(vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p));
        b.mapped = static_cast<uint8_t*>(p);
    }
    VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    bai.buffer = b.buffer;
    b.address = vkGetBufferDeviceAddress(device, &bai);
    return b;
}

void Context::destroy_buffer(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device, b.memory, nullptr);
    b = Buffer{};
}

VkDeviceMemory Context::allocate_image_memory(VkImage image) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory mem;
    VK_CHECK(vkAllocateMemory(device, &mai, nullptr, &mem));
    VK_CHECK(vkBindImageMemory(device, image, mem, 0));
    return mem;
}

// ---------------------------------------------------------- guest memory

void GuestMemory::init(Context& c) {
    struct Range {
        uint32_t base, size;
    };
    // MEM2 holds the title's data and heaps, MEM1 its fast memory; together
    // they hold every buffer the GPU reads.
    const Range ranges[] = {{layout::kMem2Base, layout::kMem2End - layout::kMem2Base},
                            {layout::kMem1Base, layout::kMem1Size}};
    for (const Range& r : ranges) {
        // Import the committed part from the region's start.
        uint32_t size = 0;
        while (size < r.size && guest_memory_committed(r.base + size, 0x10000)) size += 0x10000;
        if (size == 0) continue;
        void* host = guest_pointer(r.base);
        VkMemoryHostPointerPropertiesEXT props{VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
        VK_CHECK(c.get_memory_host_pointer_properties(c.device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                     host, &props));
        Region region{r.base, size, {}};
        VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.pNext = &ext;
        bci.size = size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
        VK_CHECK(vkCreateBuffer(c.device, &bci, nullptr, &region.buffer.buffer));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(c.device, region.buffer.buffer, &req);
        VkImportMemoryHostPointerInfoEXT import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT};
        import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        import.pHostPointer = host;
        VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
        flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        flags.pNext = &import;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &flags;
        mai.allocationSize = size;
        mai.memoryTypeIndex = c.memory_type(props.memoryTypeBits & req.memoryTypeBits, 0);
        VK_CHECK(vkAllocateMemory(c.device, &mai, nullptr, &region.buffer.memory));
        VK_CHECK(vkBindBufferMemory(c.device, region.buffer.buffer, region.buffer.memory, 0));
        VkBufferDeviceAddressInfo bai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
        bai.buffer = region.buffer.buffer;
        region.buffer.address = vkGetBufferDeviceAddress(c.device, &bai);
        region.buffer.size = size;
        std::fprintf(stderr, "ttt2: GPU: guest memory 0x%08X-0x%08X imported\n", r.base, r.base + size - 1);
        regions_.push_back(region);
    }
}

bool GuestMemory::locate(uint32_t address, uint32_t size, VkBuffer& buffer, VkDeviceSize& offset) const {
    for (const Region& r : regions_) {
        if (address >= r.base && uint64_t{address} + size <= uint64_t{r.base} + r.size) {
            buffer = r.buffer.buffer;
            offset = address - r.base;
            return true;
        }
    }
    return false;
}

VkDeviceAddress GuestMemory::device_address(uint32_t address, uint32_t& available) const {
    for (const Region& r : regions_) {
        if (address >= r.base && address - r.base < r.size) {
            available = r.size - (address - r.base);
            return r.buffer.address + (address - r.base);
        }
    }
    available = 0;
    return 0;
}

// ------------------------------------------------------------ upload ring

void UploadRing::init(Context& c, VkDeviceSize size) {
    buffer_ = c.create_buffer(size,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
}

uint8_t* UploadRing::allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize& offset) {
    const VkDeviceSize base = chunk_ * chunk_size();
    const VkDeviceSize start = (base + used_ + alignment - 1) / alignment * alignment;
    if (start + size > base + chunk_size()) return nullptr;
    used_ = start + size - base;
    offset = start;
    return buffer_.mapped + start;
}

} // namespace cafe::gpu::vk
