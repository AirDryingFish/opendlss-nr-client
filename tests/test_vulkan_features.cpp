// Exercise the real shared probe and logical-device feature chain without a GPU.
#include "vk_context.h"
#include "../client/vulkan_probe.h"
#include <algorithm>
#include <cassert>
#include <iostream>

static bool featureSupported = false, featureQueried = false;
static void VKAPI_CALL properties(VkPhysicalDevice, VkPhysicalDeviceProperties2* p) {
  p->properties.vendorID=0x10de;
  p->properties.apiVersion=VK_API_VERSION_1_3;
  for(auto* next=static_cast<VkBaseOutStructure*>(p->pNext);next;next=next->pNext) {
    if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES)
      reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(next)->subgroupSize=32;
    if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_PROPERTIES_NV) {
      auto* cuda=reinterpret_cast<VkPhysicalDeviceCudaKernelLaunchPropertiesNV*>(next);
      cuda->computeCapabilityMajor=8;cuda->computeCapabilityMinor=9;
    }
  }
}
static VkResult VKAPI_CALL extensions(VkPhysicalDevice,const char*,uint32_t* count,VkExtensionProperties* out) {
  if(out) std::strcpy(out[0].extensionName,"VK_NV_cuda_kernel_launch");
  *count=1; return VK_SUCCESS;
}
static void VKAPI_CALL queues(VkPhysicalDevice,uint32_t* count,VkQueueFamilyProperties* out) {
  if(out) { out[0]={};out[0].queueCount=1;out[0].queueFlags=VK_QUEUE_GRAPHICS_BIT|VK_QUEUE_COMPUTE_BIT;out[0].timestampValidBits=64; }
  *count=1;
}
static void VKAPI_CALL features(VkPhysicalDevice,VkPhysicalDeviceFeatures2* p) {
  featureQueried=false;
  for(auto* next=static_cast<VkBaseOutStructure*>(p->pNext);next;next=next->pNext) {
    if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV) {
      featureQueried=true;
      reinterpret_cast<VkPhysicalDeviceCudaKernelLaunchFeaturesNV*>(next)->cudaKernelLaunchFeatures=featureSupported;
    }
  }
}
PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2=properties;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties=extensions;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties=queues;
PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2=features;
int main() {
  auto unsupported=nrclient::probe(VK_NULL_HANDLE);
  assert(featureQueried);
  assert(unsupported.device.extensions.contains("VK_NV_cuda_kernel_launch"));
  assert(!unsupported.device.features.contains("cudaKernelLaunchFeatures"));
  assert(std::find(unsupported.missing.begin(),unsupported.missing.end(),"feature: cudaKernelLaunchFeatures")!=unsupported.missing.end());
  featureSupported=true;
  auto supported=nrclient::probe(VK_NULL_HANDLE);
  assert(featureQueried && supported.device.features.contains("cudaKernelLaunchFeatures"));
  assert(std::find(supported.missing.begin(),supported.missing.end(),"feature: cudaKernelLaunchFeatures")==supported.missing.end());
  vk::DeviceRequirements requirements;
  bool enabled=false;
  for(auto* next=static_cast<VkBaseOutStructure*>(requirements.pNextChain());next;next=next->pNext) {
    if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUDA_KERNEL_LAUNCH_FEATURES_NV)
      enabled=reinterpret_cast<VkPhysicalDeviceCudaKernelLaunchFeaturesNV*>(next)->cudaKernelLaunchFeatures==VK_TRUE;
  }
  assert(enabled);
  std::cout << "Vulkan CUDA launch feature: query, absent-feature rejection, enable passed\n";
}
