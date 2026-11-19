#pragma once
#include <cstdint>

// Matches NrParams in shaders/nr_common.glsl, used as a uniform or as push constants.
struct NrShaderParams {
  uint32_t fullWidth, fullHeight, validWidth, validHeight;
  uint32_t sourceWidth, sourceHeight;
  float autoMask, paperWhite, colorStrength, intensity, localTone, localStructure, skinStructure, style;
  uint32_t historyValid, seed, nrEnabled, pad0;
  float blendScale, pad1;
  float styleExposure, styleContrast, styleGamma, styleSaturation;
  float styleHue, styleVibrance, styleStrength;
  uint32_t styleMode;
};
static_assert(sizeof(NrShaderParams) == 112);
