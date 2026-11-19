#include "live_nr.h"

#include <array>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "nr_graph.h"
#include "nr_shader_params.h"
#include "reference.h"

namespace {
enum Timestamp { UploadStart, NetworkStart, NetworkEnd, ReadbackEnd, TimestampCount };

nr::Geometry screenGeometry(uint32_t width, uint32_t height) {
  if (!width || !height || uint64_t(width) * height > UINT32_MAX / 16)
    throw std::runtime_error("screen dimensions exceed the native NR tensor index range");
  auto geometry = nr::Geometry::fromValid(width, height);
  if (uint64_t(geometry.fullWidth) * geometry.fullHeight > UINT32_MAX / 16)
    throw std::runtime_error("padded screen dimensions exceed the native NR tensor index range");
  return geometry;
}
}  // namespace

struct LiveNr::Impl {
  Impl(uint32_t width, uint32_t height, const std::string& modelDir, const std::string& kernelDir)
      : geometry(screenGeometry(width, height)), bytes(VkDeviceSize(width) * height * 4),
        model(context, modelDir, false), kernels(context, kernelDir) {}

  void initialize(const std::string& shaderDir) {
    kernels.setSiluTable(ref::siluTable());
    const VkDevice device = context.device();
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.queueFamilyIndex = context.queueFamily();
    VK_CHECK(vkCreateCommandPool(device, &poolInfo, nullptr, &pool));
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocation.commandPool = pool;
    allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocation.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocation, &commands));
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    timestamps = context.createTimestampPool(TimestampCount);

    upload = context.createBuffer(bytes, true, "screen upload");
    input = context.createBuffer(bytes, false, "screen input");
    output = context.createBuffer(bytes, false, "screen output");
    previousInput = context.createBuffer(bytes, false, "previous screen input");
    previousOutput = context.createBuffer(bytes, false, "previous stabilized screen output");
    historyControl = context.createBuffer(sizeof(uint32_t), true, "screen history validity");
    readback = context.createBuffer(bytes, true, "screen readback", 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    preprocess = context.createComputePipeline(context.loadShaderModule(shaderDir + "/live_preprocess.comp.spv"),
                                               {}, "screen preprocess", 0);
    composite = context.createComputePipeline(context.loadShaderModule(shaderDir + "/live_composite.comp.spv"),
                                              {}, "screen composite", 0);
    stabilize = context.createComputePipeline(context.loadShaderModule(shaderDir + "/live_stabilize.comp.spv"),
                                              {}, "screen temporal stabilization", 0);
    record();
  }

  ~Impl() {
    const VkDevice device = context.device();
    vkDeviceWaitIdle(device);
    vkDestroyCommandPool(device, pool, nullptr);
    vkDestroyFence(device, fence, nullptr);
    vkDestroyQueryPool(device, timestamps, nullptr);
    context.destroyPipeline(preprocess);
    context.destroyPipeline(composite);
    context.destroyPipeline(stabilize);
    for (auto* b : {&upload, &input, &output, &readback, &previousInput, &previousOutput, &historyControl})
      context.destroyBuffer(*b);
  }

  void dispatch(const vk::Pipeline& pipeline, const nr::Activation& tensor, const NrShaderParams& params,
                uint32_t width, uint32_t height) {
    const vk::Buffer* bindings[vk::kGenericBindings]{};
    bindings[0] = &input;
    bindings[3] = &tensor.buffer;
    bindings[5] = &output;
    const auto set = context.allocateSet(bindings);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(commands, context.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params), &params);
    vkCmdDispatch(commands, (width + 7) / 8, (height + 7) / 8, 1);
  }

  void record() {
    // The caller has waited for the last submission. A watchdog retry reuses the model, kernels and
    // staging buffers, but the graph owns its route selection and must be rebuilt with chaining off.
    VK_CHECK(vkResetCommandPool(context.device(), pool, 0));
    context.resetDescriptorPool(0);
    graph.reset();
    graph = std::make_unique<nr::Graph>(context, model, kernels, geometry, nr::Graph::Options{});
    const auto* features = graph->allocate("screen features", geometry.fullWidth * geometry.fullHeight, 16, nr::Format::F32);
    NrShaderParams params{};
    params.fullWidth = geometry.fullWidth;
    params.fullHeight = geometry.fullHeight;
    params.sourceWidth = params.validWidth = geometry.validWidth;
    params.sourceHeight = params.validHeight = geometry.validHeight;
    params.autoMask = params.paperWhite = params.colorStrength = params.intensity = 1.0f;
    params.localTone = params.localStructure = 1.0f;
    params.skinStructure = -1.0f;
    params.nrEnabled = 1;
    // Fixed seed keeps static screen content stable. History lanes use the current proxy.

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(commands, &begin));
    vkCmdResetQueryPool(commands, timestamps, 0, TimestampCount);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestamps, UploadStart);
    VkBufferCopy copy{0, 0, bytes};
    vkCmdCopyBuffer(commands, upload.buffer, input.buffer, 1, &copy);
    context.transferBarrier(commands);
    dispatch(preprocess, *features, params, geometry.fullWidth, geometry.fullHeight);
    context.computeBarrier(commands);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, NetworkStart);
    graph->record(commands, *features);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, NetworkEnd);
    dispatch(composite, graph->head(), params, geometry.validWidth, geometry.validHeight);
    context.computeBarrier(commands);
    const vk::Buffer* temporalBindings[vk::kGenericBindings] = {&input, &previousInput, &previousOutput,
                                                                 &historyControl, nullptr, &output};
    const auto temporalSet = context.allocateSet(temporalBindings);
    vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, stabilize.pipeline);
    vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, context.pipelineLayout(), 0, 1,
                            &temporalSet, 0, nullptr);
    const uint32_t size[] = {geometry.validWidth, geometry.validHeight};
    vkCmdPushConstants(commands, context.pipelineLayout(), VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(size), size);
    vkCmdDispatch(commands, (size[0] + 7) / 8, (size[1] + 7) / 8, 1);
    context.transferBarrier(commands);
    vkCmdCopyBuffer(commands, output.buffer, readback.buffer, 1, &copy);
    vkCmdCopyBuffer(commands, input.buffer, previousInput.buffer, 1, &copy);
    vkCmdCopyBuffer(commands, output.buffer, previousOutput.buffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, nullptr, 0, nullptr);
    vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, timestamps, ReadbackEnd);
    VK_CHECK(vkEndCommandBuffer(commands));
  }

  void submit() {
    VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commands;
    VK_CHECK(vkResetFences(context.device(), 1, &fence));
    VK_CHECK(vkQueueSubmit(context.queue(), 1, &submitInfo, fence));
    VK_CHECK(vkWaitForFences(context.device(), 1, &fence, VK_TRUE, UINT64_MAX));
  }

  // Reject whole-frame discontinuities before tile matching. Local source-color checks in
  // the shader also reject disocclusions or cuts affecting just part of the captured area.
  bool keepHistory(const uint8_t* rgba) {
    constexpr unsigned columns = 32, rows = 18;
    unsigned changed = 0;
    for (unsigned y = 0; y < rows; ++y) {
      const uint32_t sy = (2 * y + 1) * geometry.validHeight / (2 * rows);
      for (unsigned x = 0; x < columns; ++x) {
        const uint32_t sx = (2 * x + 1) * geometry.validWidth / (2 * columns);
        const uint8_t* pixel = rgba + 4 * (size_t(sy) * geometry.validWidth + sx);
        auto& last = probes[y * columns + x];
        int delta = 0;
        for (unsigned c = 0; c < 3; ++c) {
          delta += std::abs(int(pixel[c]) - int(last[c]));
          last[c] = pixel[c];
        }
        if (delta > 3 * 36) ++changed;
      }
    }
    return historyValid && changed < probes.size() * 3 / 5;
  }

  const uint8_t* process(const uint8_t* rgba, bool enabled) {
    if (!rgba) throw std::runtime_error("empty RGBA screen frame");
    if (!enabled) {
      networkMs = gpuMs = 0;
      historyValid = false;
      return rgba;
    }
    *static_cast<uint32_t*>(historyControl.mapped) = keepHistory(rgba) ? 1u : 0u;
    std::memcpy(upload.mapped, rgba, bytes);
    submit();
    if (auto timeouts = kernels.chainTimeouts(); timeouts.waits) {
      fprintf(stderr, "[live] native NR watchdog at %s, retrying this frame with barriers\n", timeouts.counter.c_str());
      nr::Kernels::setChainEnabled(false);
      kernels.resetChainTimeouts();
      *static_cast<uint32_t*>(historyControl.mapped) = 0;
      record();
      submit();
      if (kernels.chainTimeouts().waits) throw std::runtime_error("native NR timed out after retry");
    }
    uint64_t ticks[TimestampCount];
    VK_CHECK(vkGetQueryPoolResults(context.device(), timestamps, 0, TimestampCount, sizeof(ticks),
                                   ticks, sizeof(ticks[0]), VK_QUERY_RESULT_64_BIT));
    const double msPerTick = context.timestampPeriodNs() * 1e-6;
    networkMs = (ticks[NetworkEnd] - ticks[NetworkStart]) * msPerTick;
    gpuMs = (ticks[ReadbackEnd] - ticks[UploadStart]) * msPerTick;
    historyValid = true;
    return static_cast<const uint8_t*>(readback.mapped);
  }

  const nr::Geometry geometry;
  const VkDeviceSize bytes;
  vk::Context context;
  nr::Model model;
  nr::Kernels kernels;
  std::unique_ptr<nr::Graph> graph;
  vk::Buffer upload, input, output, readback, previousInput, previousOutput, historyControl;
  vk::Pipeline preprocess, composite, stabilize;
  bool historyValid = false;
  std::array<std::array<uint8_t, 3>, 32 * 18> probes{};
  VkCommandPool pool{};
  VkCommandBuffer commands{};
  VkFence fence{};
  VkQueryPool timestamps{};
  double networkMs = 0, gpuMs = 0;
};

LiveNr::LiveNr(uint32_t width, uint32_t height, const std::string& modelDir, const std::string& kernelDir,
               const std::string& demoShaderDir)
    : impl_(std::make_unique<Impl>(width, height, modelDir, kernelDir)) {
  impl_->initialize(demoShaderDir);
}

LiveNr::~LiveNr() = default;

const uint8_t* LiveNr::process(const uint8_t* rgba, bool enabled) { return impl_->process(rgba, enabled); }
void LiveNr::resetHistory() { impl_->historyValid = false; }
const std::string& LiveNr::deviceName() const { return impl_->context.deviceName(); }
double LiveNr::networkMs() const { return impl_->networkMs; }
double LiveNr::gpuMs() const { return impl_->gpuMs; }
