#pragma once

#include "live_framegen.h"
#include <string>

std::unique_ptr<LiveInterpolator> makeLiveInterpolator(const std::string& backend, unsigned width,
                                                      unsigned height, unsigned multiplier);
void probeLiveProcessing(const std::string& feature, unsigned multiplier, const std::string& model,
                         const std::string& kernels, const std::string& shaders);
