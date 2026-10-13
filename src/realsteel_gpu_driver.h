// Android: an optional, player-chosen Vulkan driver (launcher: "Graphics
// driver"). Off unless the player picked a driver file and accepted the
// warning; the launcher then sets REALSTEEL_GPU_DRIVER for the game.
#pragma once

namespace realsteel::gpu_driver {

// Before the renderer starts: with a chosen driver, open it through
// libadrenotools and hand its vkGetInstanceProcAddr to the renderer. Without
// one (the default) nothing is loaded. Logs what it did either way.
void Apply();

}  // namespace realsteel::gpu_driver
