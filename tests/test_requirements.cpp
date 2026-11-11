#include "../client/requirements.h"
#include <cassert>
#include <iostream>
int main() {
 using namespace nrclient;
 Device d; d.name="test NVIDIA"; d.vendorId=0x10de; d.apiMajor=1; d.apiMinor=3;
 d.graphicsComputeQueue=true; d.subgroupSize=32;
 d.extensions.insert(requiredExtensions().begin(),requiredExtensions().end());
 d.features.insert(requiredFeatures().begin(),requiredFeatures().end());
 assert(missingRequirements(d).empty());
 d.extensions.erase("VK_EXT_shader_float8");
 assert(missingRequirements(d)==std::vector<std::string>{"extension: VK_EXT_shader_float8"});
 d.extensions.insert("VK_EXT_shader_float8"); d.features.erase("shaderFloat8CooperativeMatrix");
 assert(missingRequirements(d)==std::vector<std::string>{"feature: shaderFloat8CooperativeMatrix"});
 d.features.insert("shaderFloat8CooperativeMatrix"); d.vendorId=0x10005;
 assert(!missingRequirements(d).empty());
 d.vendorId=0x10de; d.subgroupSize=64; assert(!missingRequirements(d).empty());
 d.subgroupSize=32; d.graphicsComputeQueue=false; assert(!missingRequirements(d).empty());
 std::cout << "requirements: 6 cases passed\n";
}
