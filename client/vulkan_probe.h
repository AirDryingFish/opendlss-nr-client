#pragma once
// Include after Vulkan/volk so this works with both loader prototypes and VK_NO_PROTOTYPES.
#include "requirements.h"
#include <stdexcept>
namespace nrclient {
namespace detail {
inline void checked(VkResult result,const char* operation) {
 if(result!=VK_SUCCESS) throw std::runtime_error(std::string(operation)+": "+std::to_string(result));
}
}
using detail::checked;
template<class T>inline void append(void*& next,T& feature){feature.pNext=next;next=&feature;}
struct Probe {nrclient::Device device;std::string driver;std::vector<std::string> missing;};
inline Probe probe(VkPhysicalDevice gpu){
 Probe result;auto& d=result.device;
 VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
 VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};properties.pNext=&subgroup;
 vkGetPhysicalDeviceProperties2(gpu,&properties);
 d.name=properties.properties.deviceName;d.vendorId=properties.properties.vendorID;
 d.apiMajor=VK_API_VERSION_MAJOR(properties.properties.apiVersion);d.apiMinor=VK_API_VERSION_MINOR(properties.properties.apiVersion);d.subgroupSize=subgroup.subgroupSize;
 const auto version=properties.properties.driverVersion;
 if(d.vendorId==0x10de)result.driver=std::to_string(version>>22)+"."+std::to_string((version>>14)&255)+"."+std::to_string((version>>6)&255)+"."+std::to_string(version&63);
 else result.driver=std::to_string(version);
 uint32_t count=0;checked(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,nullptr),"enumerate extensions");
 std::vector<VkExtensionProperties> extensions(count);checked(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&count,extensions.data()),"enumerate extensions");
 for(const auto& extension:extensions)d.extensions.insert(extension.extensionName);
 if(d.extensions.contains("VK_NV_cuda_kernel_launch")){
  VkPhysicalDeviceCudaKernelLaunchPropertiesNV cuda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_PROPERTIES_NV};
  properties.pNext=&cuda;vkGetPhysicalDeviceProperties2(gpu,&properties);
  d.cudaMajor=cuda.computeCapabilityMajor;d.cudaMinor=cuda.computeCapabilityMinor;
 }
 vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,nullptr);std::vector<VkQueueFamilyProperties> queues(count);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&count,queues.data());
 for(const auto& q:queues)if(q.queueCount && (q.queueFlags&VK_QUEUE_GRAPHICS_BIT) && (q.queueFlags&VK_QUEUE_COMPUTE_BIT) && q.timestampValidBits)d.graphicsComputeQueue=true;
 VkPhysicalDeviceFeatures2 base{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
 VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
 VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
 VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
 VkPhysicalDeviceCooperativeMatrixFeaturesKHR coop{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR};
 VkPhysicalDeviceCooperativeMatrix2FeaturesNV coop2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_2_FEATURES_NV};
 VkPhysicalDeviceShaderFloat8FeaturesEXT fp8{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_FLOAT8_FEATURES_EXT};
 VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR executable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR};
 VkPhysicalDeviceShaderClockFeaturesKHR clock{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_CLOCK_FEATURES_KHR};
 VkPhysicalDeviceShaderSMBuiltinsFeaturesNV sm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_FEATURES_NV};
 append(base.pNext,f11);append(base.pNext,f12);append(base.pNext,f13);
 if(d.extensions.contains("VK_KHR_cooperative_matrix"))append(base.pNext,coop);
 if(d.extensions.contains("VK_NV_cooperative_matrix2"))append(base.pNext,coop2);
 if(d.extensions.contains("VK_EXT_shader_float8"))append(base.pNext,fp8);
 if(d.extensions.contains("VK_KHR_pipeline_executable_properties"))append(base.pNext,executable);
 if(d.extensions.contains("VK_KHR_shader_clock"))append(base.pNext,clock);
 if(d.extensions.contains("VK_NV_shader_sm_builtins"))append(base.pNext,sm);
 vkGetPhysicalDeviceFeatures2(gpu,&base);
#define FEATURE(group,name) if(group.name)d.features.insert(#name)
 FEATURE(f11,storageBuffer16BitAccess);FEATURE(f11,uniformAndStorageBuffer16BitAccess);
 FEATURE(f12,storageBuffer8BitAccess);FEATURE(f12,uniformAndStorageBuffer8BitAccess);FEATURE(f12,shaderFloat16);FEATURE(f12,shaderInt8);
 FEATURE(f12,vulkanMemoryModel);FEATURE(f12,vulkanMemoryModelDeviceScope);FEATURE(f12,hostQueryReset);FEATURE(f12,bufferDeviceAddress);
 FEATURE(f13,subgroupSizeControl);FEATURE(f13,computeFullSubgroups);FEATURE(f13,synchronization2);FEATURE(f13,maintenance4);
 FEATURE(coop,cooperativeMatrix);FEATURE(coop2,cooperativeMatrixWorkgroupScope);FEATURE(coop2,cooperativeMatrixFlexibleDimensions);
 FEATURE(coop2,cooperativeMatrixReductions);FEATURE(coop2,cooperativeMatrixConversions);FEATURE(coop2,cooperativeMatrixPerElementOperations);
 FEATURE(coop2,cooperativeMatrixTensorAddressing);FEATURE(coop2,cooperativeMatrixBlockLoads);
 FEATURE(fp8,shaderFloat8);FEATURE(fp8,shaderFloat8CooperativeMatrix);FEATURE(executable,pipelineExecutableInfo);
 FEATURE(clock,shaderSubgroupClock);FEATURE(clock,shaderDeviceClock);FEATURE(sm,shaderSMBuiltins);
 FEATURE(base.features,shaderInt16);FEATURE(base.features,shaderInt64);
#undef FEATURE
 result.missing=nrclient::missingRequirements(d);return result;
}
inline int selectPhysicalDevice(const std::vector<VkPhysicalDevice>& devices) {
 return firstEligibleDevice(devices, [](VkPhysicalDevice gpu) { return probe(gpu).device; });
}
} // namespace nrclient
