#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>
#include "vulkan_probe.h"
#include <algorithm>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void checked(VkResult result,const char* operation){if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+": "+std::to_string(result));}
std::string quote(const std::string& value){std::ostringstream out;out<<'"';for(unsigned char c:value){switch(c){case'"':out<<"\\\"";break;case'\\':out<<"\\\\";break;case'\n':out<<"\\n";break;default:if(c<32)out<<"\\u"<<std::hex<<std::setw(4)<<std::setfill('0')<<unsigned(c);else out<<char(c);}}out<<'"';return out.str();}

}
int main(int argc,char** argv){
 bool json=false;for(int i=1;i<argc;++i){if(std::string(argv[i])=="--json")json=true;else{std::cerr<<"Usage: nr-doctor [--json]\n";return 1;}}
 VkInstance instance=VK_NULL_HANDLE;
 try{
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="OpenDLSS-NR capability check";app.apiVersion=VK_API_VERSION_1_3;
  VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
  checked(vkCreateInstance(&info,nullptr,&instance),"create Vulkan 1.3 instance");
  uint32_t count=0;checked(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate GPUs");std::vector<VkPhysicalDevice> devices(count);
  checked(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate GPUs");std::vector<nrclient::Probe> results;
  for(auto gpu:devices)results.push_back(nrclient::probe(gpu));
  const int selected=nrclient::firstEligibleDevice(results, [](const auto& result){return result.device;});
  if(json){std::cout<<"{\"supported\":"<<(selected>=0?"true":"false")<<",\"backend\":\"native-vulkan-ptx-tensor-core\",\"selectedDevice\":"<<selected<<",\"devices\":[";
   for(size_t i=0;i<results.size();++i){if(i)std::cout<<',';const auto& p=results[i];std::cout<<"{\"name\":"<<quote(p.device.name)<<",\"driver\":"<<quote(p.driver)<<",\"vendorId\":"<<p.device.vendorId<<",\"missing\":[";for(size_t j=0;j<p.missing.size();++j){if(j)std::cout<<',';std::cout<<quote(p.missing[j]);}std::cout<<"]}";}std::cout<<"]}\n";
  }else{
   const bool hasNvidia=std::any_of(results.begin(),results.end(),[](const auto& p){return p.device.vendorId==0x10de;});
   for(const auto& p:results){if(hasNvidia&&p.device.vendorId!=0x10de)continue;std::cout<<p.device.name<<" / driver "<<p.driver<<" / CUDA capability "<<p.device.cudaMajor<<'.'<<p.device.cudaMinor<<'\n';if(p.missing.empty())std::cout<<"  Native NR prerequisites supported. GPU execution is not benchmarked by this check.\n";else for(const auto& missing:p.missing)std::cout<<"  Missing: "<<missing<<'\n';}
   if(selected<0)std::cout<<"Native NR cannot start. Install a driver exposing the required capabilities, reboot, then rerun scripts/check-gpu.sh. No fallback was used.\n";
  }
  vkDestroyInstance(instance,nullptr);return selected>=0?0:2;
 }catch(const std::exception& e){if(instance)vkDestroyInstance(instance,nullptr);if(json)std::cout<<"{\"supported\":false,\"error\":"<<quote(e.what())<<"}\n";else std::cerr<<e.what()<<'\n';return 1;}
}
