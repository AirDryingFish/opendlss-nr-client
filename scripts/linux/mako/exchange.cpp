#include "exchange.h"
#include "mako_copy.h"
#include <vulkan/vulkan.hpp>
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace {
constexpr auto fdMemory = vk::ExternalMemoryHandleTypeFlagBits::eOpaqueFd;
constexpr auto fdTimeline = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd;
constexpr vk::ImageSubresourceRange color{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1};
constexpr auto storageUsage = vk::ImageUsageFlagBits::eStorage | vk::ImageUsageFlagBits::eSampled;
} // namespace

struct MakoExchange::Impl {
    unsigned width, height;
    size_t bytes, stagingStride = 0;
    vk::UniqueInstance instance;
    vk::PhysicalDevice gpu;
    std::array<uint8_t, 16> uuid{};
    uint32_t family = 0;
    bool halfStorage = false;
    vk::UniqueDevice device;
    vk::Queue queue;
    vk::UniqueDeviceMemory memory;
    vk::UniqueBuffer staging;
    uint8_t* mapped = nullptr;
    struct Image {
        vk::UniqueDeviceMemory memory;
        vk::UniqueImage image;
        vk::UniqueImageView view;
    };
    std::vector<Image> images; // Two real inputs followed by the generated phases.
    vk::UniqueSemaphore timeline;
    vk::UniqueCommandPool pool;
    vk::CommandBuffer cmd;
    vk::UniqueFence fence;
    vk::UniqueDescriptorSetLayout setLayout;
    vk::UniqueDescriptorPool descriptors;
    std::vector<vk::DescriptorSet> sets;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
    PFN_vkGetMemoryFdKHR getMemoryFd = nullptr;
    PFN_vkGetSemaphoreFdKHR getSemaphoreFd = nullptr;

    Impl(unsigned w, unsigned h, unsigned outputCount) : width(w), height(h), bytes(size_t(w) * h * 4), images(2 + outputCount) {
        if (!w || !h || w > 8192 || h > 8192 || uint64_t(w) * h > 8294400)
            throw std::runtime_error("MAKO FG supports at most 8192 pixels per axis and 8294400 total pixels");
        vk::ApplicationInfo app("Live MAKO FG", 1, nullptr, 0, VK_API_VERSION_1_1);
        instance = vk::createInstanceUnique(vk::InstanceCreateInfo({}, &app));
        selectGpu();
        const float priority = 1;
        vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
        vk::PhysicalDeviceTimelineSemaphoreFeatures supportedTimeline(true);
        const char* extensions[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
        vk::DeviceCreateInfo create({}, 1, &queueInfo, 0, nullptr, 3, extensions);
        create.pNext = &supportedTimeline;
        device = gpu.createDeviceUnique(create);
        queue = device->getQueue(family, 0);
        getMemoryFd = reinterpret_cast<PFN_vkGetMemoryFdKHR>(device->getProcAddr("vkGetMemoryFdKHR"));
        getSemaphoreFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(device->getProcAddr("vkGetSemaphoreFdKHR"));
        if (!getMemoryFd || !getSemaphoreFd) throw std::runtime_error("MAKO external FD support unavailable");
        for (auto& image : images) createImage(image);
        createStaging();
        vk::ExportSemaphoreCreateInfo exportInfo(fdTimeline);
        vk::SemaphoreTypeCreateInfo type(vk::SemaphoreType::eTimeline, 0, &exportInfo);
        timeline = device->createSemaphoreUnique(vk::SemaphoreCreateInfo({}, &type));
        pool = device->createCommandPoolUnique(vk::CommandPoolCreateInfo({}, family));
        cmd = device->allocateCommandBuffers({*pool, vk::CommandBufferLevel::ePrimary, 1}).front();
        fence = device->createFenceUnique({});
        createPipeline();
    }

    ~Impl() {
        if (mapped) device->unmapMemory(*memory);
    }

    void selectGpu() {
        for (const auto& candidate : instance->enumeratePhysicalDevices()) {
            const auto properties = candidate.getProperties();
            if (properties.deviceType == vk::PhysicalDeviceType::eCpu) continue;
            const auto extensions = candidate.enumerateDeviceExtensionProperties();
            auto has = [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(),
                    [&](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
            };
            if (!has(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) || !has(VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME) ||
                !has(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME) || !has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) continue;
            vk::PhysicalDevice16BitStorageFeatures storage;
            vk::PhysicalDeviceTimelineSemaphoreFeatures timelineFeature;
            timelineFeature.pNext = &storage;
            vk::PhysicalDeviceFeatures2 features;
            features.pNext = &timelineFeature;
            candidate.getFeatures2(&features);
            if (!timelineFeature.timelineSemaphore) continue;
            const auto families = candidate.getQueueFamilyProperties();
            auto compute = std::find_if(families.begin(), families.end(), [](const auto& q) {
                return bool(q.queueFlags & vk::QueueFlagBits::eCompute);
            });
            if (compute == families.end()) continue;
            gpu = candidate;
            halfStorage = storage.storageBuffer16BitAccess;
            family = uint32_t(compute - families.begin());
            if (properties.deviceType == vk::PhysicalDeviceType::eDiscreteGpu) break;
        }
        if (!gpu) throw std::runtime_error("No Vulkan GPU with the external FD and timeline features required by MAKO FG");
        vk::PhysicalDeviceIDProperties ids;
        vk::PhysicalDeviceProperties2 props;
        props.pNext = &ids;
        gpu.getProperties2(&props);
        std::copy(ids.deviceUUID.begin(), ids.deviceUUID.end(), uuid.begin());
    }

    uint32_t memoryType(uint32_t bits, vk::MemoryPropertyFlags required, vk::MemoryPropertyFlags preferred = {}) const {
        const auto props = gpu.getMemoryProperties();
        uint32_t fallback = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            const auto flags = props.memoryTypes[i].propertyFlags;
            if (!(bits & (1u << i)) || (flags & required) != required) continue;
            if ((flags & preferred) == preferred) return i;
            fallback = i;
        }
        if (fallback == UINT32_MAX) throw std::runtime_error("MAKO frame exchange has no compatible Vulkan memory");
        return fallback;
    }

    void createImage(Image& img) {
        vk::ExternalMemoryImageCreateInfo external(fdMemory);
        vk::ImageCreateInfo create({}, vk::ImageType::e2D, vk::Format::eR8G8B8A8Unorm, {width,height,1}, 1, 1,
            vk::SampleCountFlagBits::e1, vk::ImageTiling::eOptimal, storageUsage);
        create.pNext = &external;
        img.image = device->createImageUnique(create);
        const auto req = device->getImageMemoryRequirements(*img.image);
        vk::MemoryDedicatedAllocateInfo dedicated(*img.image);
        vk::ExportMemoryAllocateInfo exportInfo(fdMemory, &dedicated);
        img.memory = device->allocateMemoryUnique({req.size, memoryType(req.memoryTypeBits,
            vk::MemoryPropertyFlagBits::eDeviceLocal), &exportInfo});
        device->bindImageMemory(*img.image, *img.memory, 0);
        img.view = device->createImageViewUnique({{}, *img.image, vk::ImageViewType::e2D,
            vk::Format::eR8G8B8A8Unorm, {}, color});
    }

    void createStaging() {
        const size_t align = gpu.getProperties().limits.minStorageBufferOffsetAlignment;
        stagingStride = (bytes + align - 1) / align * align;
        staging = device->createBufferUnique({{}, stagingStride * (images.size() - 2) + bytes, vk::BufferUsageFlagBits::eStorageBuffer});
        const auto req = device->getBufferMemoryRequirements(*staging);
        const auto required = vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent;
        memory = device->allocateMemoryUnique({req.size, memoryType(req.memoryTypeBits, required, vk::MemoryPropertyFlagBits::eHostCached)});
        device->bindBufferMemory(*staging, *memory, 0);
        mapped = static_cast<uint8_t*>(device->mapMemory(*memory, 0, VK_WHOLE_SIZE));
    }

    void createPipeline() {
        constexpr auto compute = vk::ShaderStageFlagBits::eCompute;
        const std::array bindings{vk::DescriptorSetLayoutBinding(0,vk::DescriptorType::eStorageImage,1,compute),
            vk::DescriptorSetLayoutBinding(1,vk::DescriptorType::eStorageBuffer,1,compute)};
        setLayout = device->createDescriptorSetLayoutUnique({{}, bindings});
        const auto count = uint32_t(images.size());
        const std::array sizes{vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, count),
            vk::DescriptorPoolSize(vk::DescriptorType::eStorageBuffer, count)};
        descriptors = device->createDescriptorPoolUnique({{}, count, sizes});
        const std::vector<vk::DescriptorSetLayout> layouts(count, *setLayout);
        sets = device->allocateDescriptorSets({*descriptors, layouts});
        for (unsigned i = 0; i < images.size(); ++i) {
            vk::DescriptorImageInfo image({}, *images[i].view, vk::ImageLayout::eGeneral);
            vk::DescriptorBufferInfo buffer(*staging, i >= 2 ? (i - 1) * stagingStride : 0, bytes);
            const std::array writes{vk::WriteDescriptorSet(sets[i],0,0,1,vk::DescriptorType::eStorageImage,&image),
                vk::WriteDescriptorSet(sets[i],1,0,1,vk::DescriptorType::eStorageBuffer,nullptr,&buffer)};
            device->updateDescriptorSets(writes, {});
        }
        const vk::PushConstantRange range(compute, 0, sizeof(uint32_t));
        layout = device->createPipelineLayoutUnique({{}, *setLayout, range});
        auto shader = device->createShaderModuleUnique({{}, sizeof(makoCopy), makoCopy});
        vk::ComputePipelineCreateInfo info({}, {{}, compute, *shader, "main"}, *layout);
        pipeline = device->createComputePipelineUnique({}, info).value;
    }

    void copyImage(unsigned slot) {
        const bool read = slot >= 2;
        constexpr auto compute = vk::PipelineStageFlagBits::eComputeShader;
        const auto access = read ? vk::AccessFlagBits::eShaderRead : vk::AccessFlagBits::eShaderWrite;
        const auto general = vk::ImageLayout::eGeneral;
        vk::ImageMemoryBarrier acquire({}, access, read ? general : vk::ImageLayout::eUndefined, general,
            read ? VK_QUEUE_FAMILY_EXTERNAL : VK_QUEUE_FAMILY_IGNORED,
            read ? family : VK_QUEUE_FAMILY_IGNORED, *images[slot].image, color);
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe, compute, {}, {}, {}, acquire);
        cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *pipeline);
        cmd.bindDescriptorSets(vk::PipelineBindPoint::eCompute, *layout, 0, sets[slot], {});
        const uint32_t direction = read;
        cmd.pushConstants(*layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(direction), &direction);
        cmd.dispatch((width + 15) / 16, (height + 15) / 16, 1);
        vk::ImageMemoryBarrier release(access, {}, general, general, family, VK_QUEUE_FAMILY_EXTERNAL, *images[slot].image, color);
        cmd.pipelineBarrier(compute, vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, release);
    }

    void submit(bool read, uint64_t value) {
        // A semaphore must order the ownership acquisition itself, in addition to the compute read.
        const vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
        vk::TimelineSemaphoreSubmitInfo timelineInfo;
        vk::SubmitInfo submit;
        submit.setCommandBuffers(cmd);
        if (read) {
            vk::MemoryBarrier hostRead(vk::AccessFlagBits::eShaderWrite, vk::AccessFlagBits::eHostRead);
            cmd.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader, vk::PipelineStageFlagBits::eHost, {}, hostRead, {}, {});
            timelineInfo.setWaitSemaphoreValues(value);
            submit.setWaitSemaphores(*timeline).setWaitDstStageMask(waitStage);
        } else {
            timelineInfo.setSignalSemaphoreValues(value);
            submit.setSignalSemaphores(*timeline);
        }
        cmd.end();
        submit.pNext = &timelineInfo;
        device->resetFences(*fence);
        queue.submit(submit, *fence);
        if (device->waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
            throw std::runtime_error("MAKO frame transfer failed");
    }

    void begin() {
        device->resetCommandPool(*pool);
        cmd.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
    }
};

MakoExchange::MakoExchange(unsigned w, unsigned h, unsigned outputCount)
    : impl_(std::make_unique<Impl>(w, h, outputCount)) {}
MakoExchange::~MakoExchange() = default;
bool MakoExchange::supportsHalfStorage() const { return impl_->halfStorage; }
std::array<uint8_t,16> MakoExchange::deviceUuid() const { return impl_->uuid; }

int MakoExchange::exportImage(unsigned i) const {
    VkMemoryGetFdInfoKHR info{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    info.memory = *impl_->images.at(i).memory;
    info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (impl_->getMemoryFd(*impl_->device, &info, &fd) != VK_SUCCESS)
        throw std::runtime_error("MAKO image memory export failed");
    return fd;
}
int MakoExchange::exportTimeline() const {
    VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    info.semaphore = *impl_->timeline;
    info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (impl_->getSemaphoreFd(*impl_->device, &info, &fd) != VK_SUCCESS)
        throw std::runtime_error("MAKO timeline export failed");
    return fd;
}
void MakoExchange::upload(unsigned slot, const uint8_t* pixels, uint64_t signal, bool first) {
    std::memcpy(impl_->mapped, pixels, impl_->bytes);
    impl_->begin();
    impl_->copyImage(slot);
    if (first) impl_->copyImage(1-slot);
    impl_->submit(false, signal);
}
void MakoExchange::download(uint8_t* pixels, uint64_t wait) {
    impl_->begin();
    for (unsigned slot = 2; slot < impl_->images.size(); ++slot) impl_->copyImage(slot);
    impl_->submit(true, wait);
    for (unsigned slot = 2; slot < impl_->images.size(); ++slot) {
        std::memcpy(pixels + (slot - 2) * impl_->bytes,
                    impl_->mapped + (slot - 1) * impl_->stagingStride, impl_->bytes);
    }
}
bool MakoExchange::lastUploadMatches(const uint8_t* pixels) const {
    return std::memcmp(impl_->mapped, pixels, impl_->bytes) == 0;
}
