#include <string>
/*
 * Copyright (c) 2026 Q2RTX FSR Vulkan contributors
 * SPDX-License-Identifier: MIT
 */

#include "ffx_vk_fsr3_3_1_5_bridge.h"
#include "ffx_vk_vulkan_compat.h"

#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>
#include <array>
#include <map>

namespace {

struct Image {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct PassProfile {
    static constexpr uint32_t QueryCount = 256;
    VkQueryPool pool = VK_NULL_HANDLE;
    std::vector<std::wstring> labels;
    bool overflow = false;
};

void profile_pass(void* user, VkCommandBuffer commands, const wchar_t* name,
                  const uint32_t dimensions[3], VkBool32 begin)
{
    auto& p = *static_cast<PassProfile*>(user);
    if (p.overflow) return;
    if (begin) {
        if (2 + 2 * (p.labels.size() + 1) > PassProfile::QueryCount) {
            p.overflow = true;
            return;
        }
        p.labels.push_back(std::wstring(name) + L" [" + std::to_wstring(dimensions[0]) +
            L"," + std::to_wstring(dimensions[1]) + L"," + std::to_wstring(dimensions[2]) + L"]");
    }
    const uint32_t query = 2 + 2 * uint32_t(p.labels.size() - 1) + (begin ? 0 : 1);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, p.pool, query);
}

struct ValidationState {
    uint32_t warnings = 0u;
    uint32_t errors = 0u;
};

VKAPI_ATTR VkBool32 VKAPI_CALL validation_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    ValidationState* state = static_cast<ValidationState*>(user);
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        ++state->warnings;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++state->errors;
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT))
        std::fprintf(stderr, "FSR3.1.6 public FI/OF validation: %s\n",
                     data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

bool has_validation_layer()
{
    uint32_t count = 0u;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkLayerProperties> layers(count);
    if (vkEnumerateInstanceLayerProperties(&count, layers.data()) != VK_SUCCESS)
        return false;
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0)
            return true;
    }
    return false;
}

bool expect(bool value, const char* what)
{
    if (!value)
        std::fprintf(stderr, "FSR3.1.6 public FI/OF smoke failure: %s\n", what);
    return value;
}

bool choose_compute_device(VkInstance instance, VkPhysicalDevice* physical, uint32_t* queueFamily)
{
    uint32_t count = 0u;
    if (vkEnumeratePhysicalDevices(instance, &count, nullptr) != VK_SUCCESS || count == 0u)
        return false;
    std::vector<VkPhysicalDevice> devices(count);
    if (vkEnumeratePhysicalDevices(instance, &count, devices.data()) != VK_SUCCESS)
        return false;
    for (VkPhysicalDevice device : devices) {
        uint32_t familyCount = 0u;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        for (uint32_t index = 0u; index < familyCount; ++index) {
            if (families[index].queueCount && (families[index].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                *physical = device;
                *queueFamily = index;
                return true;
            }
        }
    }
    return false;
}

bool has_extension(VkPhysicalDevice physical, const char* required)
{
    uint32_t count = 0u;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS)
        return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS)
        return false;
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, required) == 0)
            return true;
    }
    return false;
}

uint32_t find_memory_type(VkPhysicalDevice physical, uint32_t bits)
{
    VkPhysicalDeviceMemoryProperties memory{};
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    for (uint32_t index = 0u; index < memory.memoryTypeCount; ++index) {
        if ((bits & (1u << index)) &&
            (memory.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return index;
    }
    return UINT32_MAX;
}

bool create_image(VkPhysicalDevice physical, VkDevice device, uint32_t width, uint32_t height,
                  VkFormat format, Image* out)
{
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1u};
    imageInfo.mipLevels = 1u;
    imageInfo.arrayLayers = 1u;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device, &imageInfo, nullptr, &out->image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device, out->image, &requirements);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = find_memory_type(physical, requirements.memoryTypeBits);
    return allocation.memoryTypeIndex != UINT32_MAX &&
           vkAllocateMemory(device, &allocation, nullptr, &out->memory) == VK_SUCCESS &&
           vkBindImageMemory(device, out->image, out->memory, 0u) == VK_SUCCESS;
}

void destroy_image(VkDevice device, Image* image)
{
    if (image->memory)
        vkFreeMemory(device, image->memory, nullptr);
    if (image->image)
        vkDestroyImage(device, image->image, nullptr);
    *image = {};
}

FfxVkFsr3_3_1_6FrameGenerationImage image_info(
    const Image& image, VkFormat format, uint32_t width, uint32_t height, VkImageLayout layout)
{
    FfxVkFsr3_3_1_6FrameGenerationImage result{};
    result.image = image.image;
    result.format = format;
    result.width = width;
    result.height = height;
    result.layout = layout;
    result.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    return result;
}

bool initialize_images(VkCommandBuffer commandBuffer, const Image* images, uint32_t count)
{
    VkImageMemoryBarrier barriers[5]{};
    for (uint32_t index = 0u; index < count; ++index) {
        barriers[index].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[index].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[index].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[index].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[index].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barriers[index].image = images[index].image;
        barriers[index].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[index].subresourceRange.levelCount = 1u;
        barriers[index].subresourceRange.layerCount = 1u;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0u, 0u, nullptr,
                         0u, nullptr, count, barriers);
    for (uint32_t index = 0u; index < count; ++index) {
        VkClearColorValue value{};
        if (index == 0u) {
            value.float32[0] = 0.25f; value.float32[1] = 0.5f;
            value.float32[2] = 0.75f; value.float32[3] = 1.0f;
        } else if (index == 1u) value.float32[0] = 0.5f;
        vkCmdClearColorImage(commandBuffer, images[index].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &value, 1u, &barriers[index].subresourceRange);
        barriers[index].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[index].dstAccessMask = index == 3u ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
        barriers[index].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[index].newLayout = index == 3u ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0u, 0u, nullptr,
                         0u, nullptr, count, barriers);
    return true;
}

bool record_frame(FfxVkFsr3_3_1_6FrameGenerationContext* context, VkCommandBuffer commandBuffer,
                  const Image* images, VkFormat colorFormat, VkFormat motionFormat,
                  FfxVkFsr3_3_1_6FrameGenerationTransferFunction transferFunction,
                  float maxLuminance,
                  uint64_t frameId, bool reset, bool initialize, bool useDistortion,
                  bool useDefaultInterpolationRect)
{
    if (initialize)
        initialize_images(commandBuffer, images, 5u);
    FfxVkFsr3_3_1_6FrameGenerationPrepareInfo prepare{};
    prepare.commandBuffer = commandBuffer;
    prepare.color = image_info(images[0], colorFormat, 1920u, 1080u,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    prepare.depth = image_info(images[1], VK_FORMAT_R32_SFLOAT, 1280u, 720u,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    prepare.motionVectors = image_info(images[2], motionFormat, 1280u, 720u,
                                       VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    prepare.renderWidth = 1280u;
    prepare.renderHeight = 720u;
    prepare.motionVectorScaleX = 1.0f;
    prepare.motionVectorScaleY = 1.0f;
    prepare.frameTimeMilliseconds = 1000.0f / 72.0f;
    prepare.minLuminance = 0.0f;
    prepare.maxLuminance = maxLuminance;
    prepare.transferFunction = transferFunction;
    prepare.cameraNear = 0.1f;
    prepare.cameraFar = 1000.0f;
    prepare.viewSpaceToMeters = 1.0f;
    prepare.cameraVerticalFovRadians = 1.0471975512f;
    prepare.cameraUp[1] = 1.0f;
    prepare.cameraRight[0] = 1.0f;
    prepare.cameraForward[2] = -1.0f;
    prepare.frameId = frameId;
    prepare.reset = reset ? VK_TRUE : VK_FALSE;
    /* The public FI/OF context owns display-sized shared images.  Its scene
     * color input therefore cannot silently change extent between a resize
     * and the caller recreating the context. */
    {
        FfxVkFsr3_3_1_6FrameGenerationPrepareInfo invalid = prepare;
        invalid.color.width = 127u;
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
        invalid = prepare;
        invalid.cameraForward[2] = std::numeric_limits<float>::quiet_NaN();
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
    }
    if (ffxVkFsr3_3_1_6FrameGenerationContextRecordPrepare(context, &prepare) !=
        FFX_VK_FSR3_3_1_6_FRAMEGEN_OK)
        return false;
    FfxVkFsr3_3_1_6FrameGenerationDispatchInfo dispatch{};
    dispatch.commandBuffer = commandBuffer;
    dispatch.color = prepare.color;
    dispatch.output = image_info(images[3], colorFormat, 1920u, 1080u,
                                 VK_IMAGE_LAYOUT_GENERAL);
    dispatch.displayWidth = 1920u;
    dispatch.displayHeight = 1080u;
    dispatch.interpolationWidth = useDefaultInterpolationRect ? 0u : 1920u;
    dispatch.interpolationHeight = useDefaultInterpolationRect ? 0u : 1080u;
    dispatch.frameTimeMilliseconds = prepare.frameTimeMilliseconds;
    dispatch.cameraNear = prepare.cameraNear;
    dispatch.cameraFar = prepare.cameraFar;
    dispatch.viewSpaceToMeters = prepare.viewSpaceToMeters;
    dispatch.cameraVerticalFovRadians = prepare.cameraVerticalFovRadians;
    dispatch.maxLuminance = maxLuminance;
    dispatch.transferFunction = transferFunction;
    dispatch.frameId = frameId;
    dispatch.reset = reset ? VK_TRUE : VK_FALSE;
    /* The public wrapper must fail closed when a host tries to reuse its
     * source image as a generated-frame target. This otherwise corrupts
     * temporal input/history in the host renderer. */
    {
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo invalid = dispatch;
        invalid.output.image = images[0].image;
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
        invalid = dispatch;
        invalid.cameraVerticalFovRadians = std::numeric_limits<float>::infinity();
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
    }
    /* A non-default interpolation rect must be non-empty and inside the
     * fixed context display extent.  These checks run before the SDK records
     * work, which prevents resize glitches from becoming bad generated frames. */
    {
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo invalid = dispatch;
        invalid.interpolationX = 1u;
        invalid.interpolationWidth = 1920u;
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
        invalid = dispatch;
        invalid.interpolationWidth = 0u;
        invalid.interpolationHeight = 1080u;
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
    }
    if (useDistortion) {
        dispatch.distortionField = image_info(images[4], VK_FORMAT_R16G16_SFLOAT, 1920u, 1080u,
                                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        /* The public field is explicitly float2 UV displacement.  Reject a
         * same-sized image with a superficially plausible but incompatible
         * format before recording the valid external-field dispatch. */
        FfxVkFsr3_3_1_6FrameGenerationDispatchInfo invalid = dispatch;
        invalid.distortionField.format = VK_FORMAT_R16_SFLOAT;
        if (ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &invalid) !=
            FFX_VK_FSR3_3_1_6_FRAMEGEN_ERROR_INVALID_ARGUMENT)
            return false;
    }
    return ffxVkFsr3_3_1_6FrameGenerationContextRecordDispatch(context, &dispatch) ==
           FFX_VK_FSR3_3_1_6_FRAMEGEN_OK;
}

} // namespace

int main(int argc, char** argv)
{
    // Full-HD Quality inputs. This checks the backend and measures its cost on a
    // deterministic synthetic frame; it is not a prediction of gameplay FPS.
    VkQueryPool timingPool = VK_NULL_HANDLE;
    std::vector<double> timingMs, recordMs;
    const bool profile = [] {const char* e=std::getenv("BB_FG_PASS_PROFILE");return e && e[0]=='1';}();
    PassProfile passProfile;
    struct PassStats {double totalMs=0; uint64_t count=0;};
    std::map<std::wstring,PassStats> passStats;
    const bool reuse = [] {const char* e=std::getenv("BB_FG_CPU_REUSE");return e && e[0]=='1';}();
    const bool benchmark = argc > 1 && std::string(argv[1]) == "--benchmark";
    uint64_t warmedRetiredBytes=0, stabilizedConstants=0, stabilizedDescriptors=0;
    const uint64_t lastFrame = argc > 1 && std::string(argv[1]) == "--stress" ? 2050u : benchmark ? 150u : 38u;
    uint64_t residentBaseline = 0u;
    uint64_t maximumPendingBytes = 0u;
    float timestampPeriod = 1.0f;

#if defined(FSR316_SMOKE_FLOAT16)
    const VkFormat colorFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#else
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
#endif
#if defined(FSR316_SMOKE_MOTION_RGBA16F)
    const VkFormat motionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
#else
    const VkFormat motionFormat = VK_FORMAT_R16G16_SFLOAT;
#endif
#if defined(FSR316_SMOKE_SCRGB)
    const FfxVkFsr3_3_1_6FrameGenerationTransferFunction transferFunction =
        FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB;
    const float maxLuminance = 1000.0f;
#else
    const FfxVkFsr3_3_1_6FrameGenerationTransferFunction transferFunction =
        FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SRGB;
    const float maxLuminance = 1.0f;
#endif
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    PFN_vkDestroyDebugUtilsMessengerEXT destroyMessenger = nullptr;
    FfxVkFsr3_3_1_6FrameGenerationContext* context = nullptr;
    Image images[5]{};
    const float priority = 1.0f;
    uint32_t family = 0u;
    int result = 1;
    ValidationState validation{};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    if (!benchmark && !has_validation_layer())
        return 77;
    VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug.pfnUserCallback = validation_callback;
    debug.pUserData = &validation;
    const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char* instanceExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
    const VkValidationFeatureEnableEXT syncEnable=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT syncValidation{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
    syncValidation.pNext=&debug;
    syncValidation.enabledValidationFeatureCount=1;
    syncValidation.pEnabledValidationFeatures=&syncEnable;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    instanceInfo.pNext = benchmark ? static_cast<void*>(&debug) : static_cast<void*>(&syncValidation);
    instanceInfo.enabledLayerCount = benchmark ? 0u : 1u;
    instanceInfo.ppEnabledLayerNames = layers;
    instanceInfo.enabledExtensionCount = benchmark ? 1u : 2u;
    instanceInfo.ppEnabledExtensionNames = instanceExtensions;
    if (vkCreateInstance(&instanceInfo, nullptr, &instance) != VK_SUCCESS ||
        !choose_compute_device(instance, &physical, &family))
        goto cleanup;
    {
        const auto createMessenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
        destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
        if (!createMessenger || !destroyMessenger ||
            createMessenger(instance, &debug, nullptr, &messenger) != VK_SUCCESS) {
            result = 77;
            goto cleanup;
        }
    }
    {
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family;
        queueInfo.queueCount = 1u;
        queueInfo.pQueuePriorities = &priority;
        VkPhysicalDeviceFeatures features{};
        VkPhysicalDeviceFeatures supported{};
        vkGetPhysicalDeviceFeatures(physical, &supported);
        features.shaderStorageImageReadWithoutFormat = supported.shaderStorageImageReadWithoutFormat;
        features.shaderStorageImageWriteWithoutFormat = supported.shaderStorageImageWriteWithoutFormat;
        if (!features.shaderStorageImageReadWithoutFormat || !features.shaderStorageImageWriteWithoutFormat) {
            result = 77;
            goto cleanup;
        }
        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR derivative{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
        VkPhysicalDeviceComputeShaderDerivativesFeaturesKHR supportedDerivative{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_KHR};
        const char* extensions[1]{};
        uint32_t extensionCount = 0u;
        if (has_extension(physical, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME)) {
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &supportedDerivative;
            vkGetPhysicalDeviceFeatures2(physical, &features2);
            if (supportedDerivative.computeDerivativeGroupLinear) {
                derivative.computeDerivativeGroupLinear = VK_TRUE;
                extensions[extensionCount++] = VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;
            }
        }
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        deviceInfo.queueCreateInfoCount = 1u;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.pEnabledFeatures = &features;
        deviceInfo.pNext = extensionCount ? &derivative : nullptr;
        deviceInfo.enabledExtensionCount = extensionCount;
        deviceInfo.ppEnabledExtensionNames = extensionCount ? extensions : nullptr;
        if (vkCreateDevice(physical, &deviceInfo, nullptr, &device) != VK_SUCCESS) {
            result = 77;
            goto cleanup;
        }
    }
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        timestampPeriod = properties.limits.timestampPeriod;
        std::printf("GPU: %s; driver=%u; timestampPeriod=%.3f ns\n",
                    properties.deviceName, properties.driverVersion, timestampPeriod);
        for (const char* extension : {"VK_NV_low_latency2", "VK_KHR_present_id", "VK_KHR_present_wait", "VK_EXT_memory_budget"})
            std::printf("Capability %s: %s\n", extension,
                        has_extension(physical, extension) ? "available" : "unavailable");
        VkQueryPoolCreateInfo queryInfo{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        queryInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
        queryInfo.queryCount = profile ? PassProfile::QueryCount : 2;
        if (vkCreateQueryPool(device, &queryInfo, nullptr, &timingPool) != VK_SUCCESS)
            goto cleanup;
    }
    for (uint32_t index = 0u; index < 5u; ++index) {
        const VkFormat formats[] = {colorFormat, VK_FORMAT_R32_SFLOAT,
                                    motionFormat, colorFormat, VK_FORMAT_R16G16_SFLOAT};
        const uint32_t sizes[] = {1920u, 1280u, 1280u, 1920u, 1920u};
        const uint32_t heights[] = {1080u, 720u, 720u, 1080u, 1080u};
        if (!expect(create_image(physical, device, sizes[index], heights[index], formats[index], &images[index]),
                    "create public API image"))
            goto cleanup;
    }
    {
        FfxVkFsr3_3_1_6FrameGenerationCreateInfo create{};
        create.physicalDevice = physical;
        create.device = device;
        create.maxRenderWidth = 1280u;
        create.maxRenderHeight = 720u;
        create.displayWidth = 1920u;
        create.displayHeight = 1080u;
        create.colorFormat = colorFormat;
        const FfxVkFsr3_3_1_6FrameGenerationResult createResult =
            ffxVkFsr3_3_1_6FrameGenerationContextCreate(&create, &context);
        if (createResult != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK)
            std::fprintf(stderr, "FSR3.1.6 public FI/OF create result: %d\n", (int)createResult);
        if (!expect(createResult == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "create public FI/OF context"))
            goto cleanup;
        FfxVkFsr3_3_1_6FrameGenerationMemoryUsage memoryUsage{};
        if (!expect(ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(
                        context, &memoryUsage) == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK &&
                    memoryUsage.totalUsageInBytes > 0u &&
                    memoryUsage.totalUsageInBytes >= memoryUsage.aliasableUsageInBytes,
                    "get public FI/OF memory usage"))
            goto cleanup;
        residentBaseline = memoryUsage.totalUsageInBytes;
        std::printf("FSR FI/OF resident: %.2f MiB (SDK and shared images, excludes application images and swapchain)\n",
                    double(memoryUsage.totalUsageInBytes) / (1024.0 * 1024.0));
    }
    {
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocation.commandPool = pool;
        allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocation.commandBufferCount = 1u;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (!expect(vkCreateCommandPool(device, &poolInfo, nullptr, &pool) == VK_SUCCESS,
                    "create public API command pool"))
            goto cleanup;
        allocation.commandPool = pool;
        if (!expect(vkAllocateCommandBuffers(device, &allocation, &commands) == VK_SUCCESS,
                    "allocate public API command buffer") ||
            !expect(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS,
                    "begin reset frame") ||
            !expect(record_frame(context, commands, images, colorFormat, motionFormat,
                                 transferFunction, maxLuminance,
                                 1u, true, true, false, false), "record reset frame") ||
            !expect(vkEndCommandBuffer(commands) == VK_SUCCESS, "end reset frame"))
            goto cleanup;
        vkGetDeviceQueue(device, family, 0u, &queue);
        submit.commandBufferCount = 1u;
        submit.pCommandBuffers = &commands;
        if (!expect(vkQueueSubmit(queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                    vkQueueWaitIdle(queue) == VK_SUCCESS, "submit reset frame") ||
            !expect(vkResetCommandPool(device, pool, 0u) == VK_SUCCESS, "reset temporal pool") ||
            !expect(vkBeginCommandBuffer(commands, &begin) == VK_SUCCESS, "begin temporal frame") ||
            !expect(record_frame(context, commands, images, colorFormat, motionFormat,
                                 transferFunction, maxLuminance,
                                 2u, false, false, true, true), "record temporal frame with distortion field") ||
            !expect(vkEndCommandBuffer(commands) == VK_SUCCESS, "end temporal frame") ||
            !expect(vkQueueSubmit(queue, 1u, &submit, VK_NULL_HANDLE) == VK_SUCCESS &&
                    vkQueueWaitIdle(queue) == VK_SUCCESS, "submit temporal frame") ||
            !expect(ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, 2u) ==
                    FFX_VK_FSR3_3_1_6_FRAMEGEN_OK, "retire reset+temporal frames"))
            goto cleanup;
    }
    {
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        if (vkResetCommandPool(device,pool,0) != VK_SUCCESS || vkBeginCommandBuffer(commands,&begin) != VK_SUCCESS) goto cleanup;
        FfxVkFsr3TransientStats pendingBefore{},pendingAfter{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetTransientStats(context,&pendingBefore);
        for (uint64_t id=3;id<=6;++id) {
            if (!record_frame(context,commands,images,colorFormat,motionFormat,
                transferFunction,maxLuminance,id,false,false,false,true)) goto cleanup;
        }
        ffxVkFsr3_3_1_6FrameGenerationContextGetTransientStats(context,&pendingAfter);
        if (!expect(pendingAfter.pendingConstants>pendingBefore.pendingConstants &&
                    pendingAfter.pendingDescriptors>pendingBefore.pendingDescriptors,
                    "four unretired frames keep independent resources")) goto cleanup;
        if (vkEndCommandBuffer(commands) != VK_SUCCESS || vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(queue) != VK_SUCCESS || ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context,6) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) goto cleanup;
        std::printf("PASS: four unretired frames, buffers=%llu sets=%llu; retired only after GPU completion\n",
            (unsigned long long)pendingAfter.pendingConstants,(unsigned long long)pendingAfter.pendingDescriptors);
        if (profile) {
            passProfile.pool=timingPool;
            passProfile.labels.reserve(PassProfile::QueryCount/2);
            ffxVkFsr3_3_1_6FrameGenerationContextSetGpuPassCallback(context,profile_pass,&passProfile);
        }
        for (uint64_t id = 7; id <= lastFrame; ++id) {
            if (vkResetCommandPool(device, pool, 0) != VK_SUCCESS ||
                vkBeginCommandBuffer(commands, &begin) != VK_SUCCESS) goto cleanup;
            passProfile.labels.clear();
            vkCmdResetQueryPool(commands, timingPool, 0, profile ? PassProfile::QueryCount : 2);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timingPool, 0);
            const auto recordStart=std::chrono::steady_clock::now();
            if (!record_frame(context, commands, images, colorFormat, motionFormat,
                              transferFunction, maxLuminance, id, false, false, false, true)) goto cleanup;
            if (id>=12) recordMs.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-recordStart).count());
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timingPool, 1);
            if (vkEndCommandBuffer(commands) != VK_SUCCESS ||
                vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS ||
                vkQueueWaitIdle(queue) != VK_SUCCESS) goto cleanup;
            uint64_t ticks[2]{};
            if (vkGetQueryPoolResults(device, timingPool, 0, 2, sizeof(ticks), ticks,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) goto cleanup;
            if (profile) {
                if (!expect(!passProfile.overflow && !passProfile.labels.empty(),"pass timestamp capacity")) goto cleanup;
                std::array<uint64_t,PassProfile::QueryCount> passTicks{};
                const uint32_t count=uint32_t(passProfile.labels.size())*2;
                if (vkGetQueryPoolResults(device,timingPool,2,count,sizeof(passTicks),passTicks.data(),
                    sizeof(uint64_t),VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT)!=VK_SUCCESS) goto cleanup;
                if (id>=12) for (size_t i=0;i<passProfile.labels.size();++i) {
                    auto& stat=passStats[passProfile.labels[i]];
                    stat.totalMs+=double(passTicks[2*i+1]-passTicks[2*i])*timestampPeriod/1000000.0;
                    ++stat.count;
                }
            }
            if (id >= 9) timingMs.push_back(double(ticks[1] - ticks[0]) * timestampPeriod / 1000000.0);
            FfxVkFsr3_3_1_6FrameGenerationMemoryUsage usage{};
            if (ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) goto cleanup;
            maximumPendingBytes = std::max(maximumPendingBytes, usage.totalUsageInBytes);
            if (ffxVkFsr3_3_1_6FrameGenerationContextRetireFrame(context, id) != FFX_VK_FSR3_3_1_6_FRAMEGEN_OK) goto cleanup;
            if (!expect(ffxVkFsr3_3_1_6FrameGenerationContextGetMemoryUsage(context, &usage) == FFX_VK_FSR3_3_1_6_FRAMEGEN_OK,
                        "query memory after completed frame")) goto cleanup;
            FfxVkFsr3TransientStats stats{};
            ffxVkFsr3_3_1_6FrameGenerationContextGetTransientStats(context,&stats);
            if (!expect(stats.cachedConstantBytes<=64u*1024u && stats.cachedConstants<=32 && stats.cachedDescriptors<=128 &&
                        stats.pendingConstants==0 && stats.pendingDescriptors==0,
                        "completed cache bounded and no pending transients")) goto cleanup;
            if (id==12) {
                warmedRetiredBytes=usage.totalUsageInBytes;
                stabilizedConstants=stats.constantsCreated;stabilizedDescriptors=stats.descriptorsCreated;
            }
            if (id>=12 && !expect(usage.totalUsageInBytes==warmedRetiredBytes &&
                (!reuse || (stats.constantsCreated==stabilizedConstants && stats.descriptorsCreated==stabilizedDescriptors &&
                    stats.constantsReused>0 && stats.descriptorsReused>0)), "no allocation growth after cache warmup")) goto cleanup;
            if ((!reuse && usage.totalUsageInBytes != residentBaseline) ||
                usage.totalUsageInBytes > residentBaseline+64u*1024u) {
                std::fprintf(stderr, "Transient allocation leak at frame %llu: baseline=%llu, after retirement=%llu bytes\n",
                             (unsigned long long)id, (unsigned long long)residentBaseline,
                             (unsigned long long)usage.totalUsageInBytes);
                goto cleanup;
            }
            if (id % 256u == 0u) {
                std::printf("Lifetime stress: %llu frames, retired=%llu, peak=%llu bytes\n",
                            (unsigned long long)id, (unsigned long long)usage.totalUsageInBytes,
                            (unsigned long long)maximumPendingBytes);
                std::fflush(stdout);
            }
        }
        if (profile) {
            ffxVkFsr3_3_1_6FrameGenerationContextSetGpuPassCallback(context,nullptr,nullptr);
            std::vector<std::pair<std::wstring,PassStats>> sorted(passStats.begin(),passStats.end());
            std::sort(sorted.begin(),sorted.end(),[](const auto& a,const auto& b){return a.second.totalMs>b.second.totalMs;});
            for (const auto& [name,stat]:sorted)
                std::printf("GPU pass %ls: mean %.6f ms, samples=%llu\n",name.c_str(),stat.totalMs/stat.count,(unsigned long long)stat.count);
        }
        std::sort(recordMs.begin(),recordMs.end());
        FfxVkFsr3TransientStats stats{};
        ffxVkFsr3_3_1_6FrameGenerationContextGetTransientStats(context,&stats);
        std::printf("CPU FI/OF record: median %.3f ms, p95 %.3f ms; %zu frames; reuse=%d, validation=%d\n",
            recordMs[recordMs.size()/2],recordMs[recordMs.size()*95/100],recordMs.size(),reuse,!benchmark);
        std::printf("Transient resources: constants created=%llu reused=%llu, descriptor sets created=%llu reused=%llu; cached %llu bytes/%llu buffers/%llu sets; stabilized resident=%llu bytes\n",
            (unsigned long long)stats.constantsCreated,(unsigned long long)stats.constantsReused,
            (unsigned long long)stats.descriptorsCreated,(unsigned long long)stats.descriptorsReused,
            (unsigned long long)stats.cachedConstantBytes,(unsigned long long)stats.cachedConstants,
            (unsigned long long)stats.cachedDescriptors,(unsigned long long)warmedRetiredBytes);
        std::printf("SDK barriers: jobs=%llu calls=%llu image deps=%llu global deps=%llu removed duplicates=%llu batched calls=%llu\n",
            (unsigned long long)stats.sdkBarrierJobs,(unsigned long long)stats.sdkBarrierCalls,
            (unsigned long long)stats.imageDependencies,(unsigned long long)stats.memoryDependencies,
            (unsigned long long)stats.duplicateDependenciesRemoved,(unsigned long long)stats.batchedBarrierCalls);
        const char* gpuBatch=std::getenv("BB_FG_GPU_BATCH");
        if (gpuBatch && gpuBatch[0]=='1' && !expect(stats.batchedBarrierCalls>0 &&
            stats.sdkBarrierCalls<stats.sdkBarrierJobs && stats.duplicateDependenciesRemoved>0,
            "adjacent barriers grouped; real memory dependencies retained")) goto cleanup;
        std::sort(timingMs.begin(), timingMs.end());
        std::printf("Full-HD FI/OF GPU timing: median %.3f ms, p95 %.3f ms, min %.3f ms; %zu synthetic frames; 72 FPS total budget=13.889 ms\n",
                    timingMs[timingMs.size()/2], timingMs[timingMs.size()*95/100], timingMs.front(), timingMs.size());
    }
    if (!expect(validation.errors == 0u, "Vulkan validation errors"))
        goto cleanup;
    std::printf("FSR3.1.6 public FI/OF Vulkan API reset+temporal smoke passed (external distortion field; %s color, %s motion, %s; validation warnings=%u)\n",
                colorFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? "RGBA16F" : "RGBA8",
                motionFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? "RGBA16F" : "RG16F",
                transferFunction == FFX_VK_FSR3_3_1_6_FRAMEGEN_TRANSFER_SCRGB ? "scRGB" : "sRGB",
                validation.warnings);
    result = validation.warnings ? 1 : 0;

cleanup:
    if (device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);
    ffxVkFsr3_3_1_6FrameGenerationContextDestroy(context);
    for (Image& image : images)
        destroy_image(device, &image);
    if (timingPool != VK_NULL_HANDLE) vkDestroyQueryPool(device, timingPool, nullptr);
    if (pool != VK_NULL_HANDLE)
        vkDestroyCommandPool(device, pool, nullptr);
    if (device != VK_NULL_HANDLE)
        vkDestroyDevice(device, nullptr);
    if (messenger != VK_NULL_HANDLE && destroyMessenger)
        destroyMessenger(instance, messenger, nullptr);
    if (instance != VK_NULL_HANDLE)
        vkDestroyInstance(instance, nullptr);
    return result;
}
