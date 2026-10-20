// Fighter import: character-select icons in the stock style - the game's own
// octagonal frame (the pixels the stock icons share) around a head-and-
// shoulders render of the fighter on the frame's background colour.
// DLC_HD.pac /MENU/SSFD/0000|0001|0002: 128x128 (player 1), 128x128
// (player 2), 128x256 (large); DXT5, one level.
#pragma once

#include "primport_model.h"

namespace rs::primport {

struct RenderMesh {
  std::vector<Vec3> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::array<int, 3>> tris;
};

// Orthographic view of the box centre +- (half_w, half_h), the model turned by
// yaw then pitch; +x = the model's left, +y down, front = -z. RGBA.
Image render(const std::vector<RenderMesh>& meshes, const Image& texture, const Image* glow, Vec3 centre, double half_w,
             double half_h, int width, int height, double yaw = 0.35, double pitch = -0.12, int ss = 3, double gain = 1.6);

// The three icons ("0000", "0001", "0002" -> DDS) for a posed fighter, framed
// on its head and shoulders. stock_dlc_hd: the game's DLC_HD.pac.
std::map<std::string, Bytes> make_icons(const Archive& stock_dlc_hd, const Posed& fighter);

}  // namespace rs::primport
