#pragma once
#include <cstdint>
#include <set>
#include <string>
#include <vector>
namespace nrclient {
struct Device { std::string name; uint32_t vendorId=0,apiMajor=0,apiMinor=0,subgroupSize=0,cudaMajor=0,cudaMinor=0; bool graphicsComputeQueue=false; std::set<std::string> extensions,features; };
inline const std::vector<std::string>& requiredExtensions() {
 static const std::vector<std::string> values{
  "VK_KHR_cooperative_matrix", "VK_EXT_shader_float8",
  "VK_KHR_pipeline_executable_properties", "VK_NV_cooperative_matrix2",
  "VK_NV_cuda_kernel_launch", "VK_KHR_shader_clock", "VK_NV_shader_sm_builtins", "VK_KHR_swapchain"};
 return values;
}
inline const std::vector<std::string>& requiredFeatures() {
 static const std::vector<std::string> values{
  "storageBuffer16BitAccess", "uniformAndStorageBuffer16BitAccess", "storageBuffer8BitAccess",
  "uniformAndStorageBuffer8BitAccess", "shaderFloat16", "shaderInt8", "vulkanMemoryModel",
  "vulkanMemoryModelDeviceScope", "hostQueryReset", "bufferDeviceAddress", "subgroupSizeControl",
  "computeFullSubgroups", "synchronization2", "maintenance4", "cooperativeMatrix",
  "cooperativeMatrixWorkgroupScope", "cooperativeMatrixFlexibleDimensions", "cooperativeMatrixReductions",
  "cooperativeMatrixConversions", "cooperativeMatrixPerElementOperations", "cooperativeMatrixTensorAddressing",
  "cooperativeMatrixBlockLoads", "shaderFloat8", "shaderFloat8CooperativeMatrix",
  "pipelineExecutableInfo", "shaderSubgroupClock", "shaderDeviceClock", "shaderSMBuiltins",
  "shaderInt16", "shaderInt64", "cudaKernelLaunchFeatures"};
 return values;
}
inline std::vector<std::string> missingRequirements(const Device& device) {
 std::vector<std::string> missing;
 if(device.vendorId!=0x10de)missing.emplace_back("NVIDIA GPU required by native PTX backend");
 if(device.cudaMajor*10+device.cudaMinor<89)missing.emplace_back("CUDA compute capability 8.9 (Ada) or newer required");
 if(device.apiMajor<1 || (device.apiMajor==1 && device.apiMinor<3))missing.emplace_back("Vulkan 1.3 required");
 if(!device.graphicsComputeQueue)missing.emplace_back("graphics + compute queue with timestamps");
 if(device.subgroupSize!=32)missing.emplace_back("subgroup size 32 required");
 for(const auto& value:requiredExtensions())if(!device.extensions.contains(value))missing.push_back("extension: "+value);
 for(const auto& value:requiredFeatures())if(!device.features.contains(value))missing.push_back("feature: "+value);
 return missing;
}
// Probe every candidate against the same predicate used by diagnostics.
template<class Handle, class ProbeDevice>
int firstEligibleDevice(const std::vector<Handle>& devices, ProbeDevice probe) {
 for (size_t i=0; i<devices.size(); ++i)
  if (missingRequirements(probe(devices[i])).empty()) return static_cast<int>(i);
 return -1;
}

}
