// Android: an optional, player-chosen Vulkan driver (see the header).
//
// libadrenotools (BSD-2-Clause, fetched at build time) loads a driver from
// the app's private storage in place of the system one. It is packaged as
// libadrenotools.so + its two hooks and only dlopen'd here when a driver was
// chosen, so with the system driver none of its code runs.

#include "realsteel_gpu_driver.h"

#include <rex/logging.h>

#include "vk_backend.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdlib>
#include <filesystem>
#include <string>
#endif

namespace realsteel::gpu_driver {

#ifdef __ANDROID__
namespace {

// adrenotools/priv.h: ADRENOTOOLS_DRIVER_CUSTOM.
constexpr int kDriverCustom = 1 << 0;
using OpenLibvulkan = void* (*)(int dlopen_mode, int feature_flags, const char* tmp_lib_dir,
                                const char* hook_lib_dir, const char* custom_driver_dir,
                                const char* custom_driver_name, const char* file_redirect_dir,
                                void** user_mapping_handle);

const char* Env(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr && *v != 0 ? v : nullptr;
}

}  // namespace

void Apply() {
  const char* driver = Env("REALSTEEL_GPU_DRIVER");
  if (driver == nullptr) {
    REXLOG_INFO("[gpu-driver] system driver (default)");
    return;
  }
  const char* label = Env("REALSTEEL_GPU_DRIVER_LABEL");
  const char* lib_dir = Env("REALSTEEL_NATIVE_LIB_DIR");
  const std::filesystem::path path(driver);
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  REXLOG_WARN("[gpu-driver] CUSTOM GPU driver chosen in the launcher: {} ({}, {} bytes)",
              label != nullptr ? label : "?", path.filename().string(),
              ec ? 0 : static_cast<unsigned long long>(size));
  if (lib_dir == nullptr || ec) {
    REXLOG_ERROR("[gpu-driver] custom driver unusable ({}); using the system driver",
                 lib_dir == nullptr ? "no native library folder" : "driver file missing");
    return;
  }
  void* tools = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);
  auto open_libvulkan =
      tools != nullptr ? reinterpret_cast<OpenLibvulkan>(dlsym(tools, "adrenotools_open_libvulkan"))
                       : nullptr;
  if (open_libvulkan == nullptr) {
    REXLOG_ERROR("[gpu-driver] libadrenotools not available ({}); using the system driver",
                 dlerror() != nullptr ? "dlopen failed" : "symbol missing");
    return;
  }
  const std::string dir = path.parent_path().string() + "/";
  const std::string name = path.filename().string();
  void* vulkan = open_libvulkan(RTLD_NOW, kDriverCustom, nullptr, lib_dir, dir.c_str(),
                                name.c_str(), nullptr, nullptr);
  void* gipa = vulkan != nullptr ? dlsym(vulkan, "vkGetInstanceProcAddr") : nullptr;
  if (gipa == nullptr) {
    REXLOG_ERROR("[gpu-driver] the custom driver did not load ({}); using the system driver",
                 vulkan == nullptr ? "adrenotools_open_libvulkan failed" : "no vkGetInstanceProcAddr");
    return;
  }
  rs::vk::SetVulkanLoader(gipa);
  REXLOG_WARN("[gpu-driver] custom driver loaded through libadrenotools");
}
#else
void Apply() {}
#endif

}  // namespace realsteel::gpu_driver
