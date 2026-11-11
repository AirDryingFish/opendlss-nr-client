// Real gltfio provider + installed uberarchive; NOOP creates no GPU device.
#include <filament/Engine.h>
#include <filament/Material.h>
#include <gltfio/MaterialProvider.h>
#include <gltfio/materials/uberarchive.h>

#include <cstdio>

using namespace filament;
using namespace filament::gltfio;

int main() {
  Engine* engine = Engine::create(Engine::Backend::NOOP);
  if (!engine) { std::fprintf(stderr, "cannot create NOOP engine\n"); return 2; }
  MaterialProvider* provider = createUbershaderProvider(
      engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
  struct Case { const char* name; bool specular, ior, clearCoat, transmission, volume; };
  const Case cases[] = {
      {"base", false, false, false, false, false},
      {"specular", true, false, false, false, false},
      {"ior", false, true, false, false, false},
      {"specular+ior", true, true, false, false, false},
      {"specular+clearcoat", true, false, true, false, false},
      {"transmission", true, true, false, true, false},
      {"transmission+volume", true, true, false, true, true},
      {"volume", true, true, false, false, true},
  };
  const AlphaMode modes[] = {AlphaMode::OPAQUE, AlphaMode::MASK, AlphaMode::BLEND};
  const BlendingMode expectedModes[] = {BlendingMode::OPAQUE, BlendingMode::MASKED, BlendingMode::FADE};
  int failures = 0;
  for (const auto& test : cases) {
    for (unsigned mode = 0; mode < 3; ++mode) {
      MaterialKey key{};
      key.alphaMode = modes[mode];
      key.doubleSided = true;
      key.hasBaseColorTexture = true;
      key.hasNormalTexture = true;
      key.hasSpecular = test.specular;
      key.hasIOR = test.ior;
      key.hasClearCoat = test.clearCoat;
      key.hasTransmission = test.transmission;
      key.hasVolume = test.volume;
      UvMap uvmap{};
      const Material* material = provider->getMaterial(&key, &uvmap, test.name);
      const auto expectedRefraction = test.transmission || test.volume
          ? RefractionMode::SCREEN_SPACE : RefractionMode::NONE;
      const bool correct = material && material->getRefractionMode() == expectedRefraction &&
          material->getBlendingMode() == expectedModes[mode];
      std::printf("%s %s alpha=%u selected=%s refraction=%u expected=%u\n",
          correct ? "PASS" : "FAIL", test.name, mode,
          material ? material->getName() : "<none>",
          material ? unsigned(material->getRefractionMode()) : 255u,
          unsigned(expectedRefraction));
      if (!correct) ++failures;
    }
  }
  provider->destroyMaterials();
  delete provider;
  Engine::destroy(&engine);
  return failures ? 1 : 0;
}
