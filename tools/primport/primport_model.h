// Fighter import: characters (skeleton + skinned meshes), the conversion of a
// fighter's body onto a Real Steel movie robot's skeleton, its textures and
// its character pack entry.
#pragma once

#include "primport_core.h"

namespace rs::primport {

struct MeshData {
  std::string group;
  std::vector<Vec3> pos, nrm;
  std::vector<std::array<double, 2>> uv;
  std::vector<std::vector<std::pair<int, double>>> skin;   // (bone, weight)
  std::vector<std::array<int, 3>> tris;
  std::vector<uint32_t> colour;
};

// A character of a character pack: entry 'entry' (default the first) of its
// first group; items 0 (JBOY model), 0x0A / 0x0C (texture packs), 0x5D
// (motion bank, Pacific Rim), ...
struct Character {
  Character(const Bytes& pack, const std::string& entry = "");
  std::map<uint32_t, Bytes> items;
  std::unique_ptr<Model> model;
  std::vector<std::string> names;
  std::vector<int> parent;
  std::vector<Mat3> world_R, local_R;
  std::vector<Vec3> world_T, local_pos;
  std::map<std::string, Bytes> textures;   // name -> DDS (items 0x0A then 0x0C)
  int bone(const std::string& name) const;   // -1 if missing
  std::vector<MeshData> meshes(bool skip_weapons) const;
};

// The character with some bones turned about their own y axes (fists) and the
// mesh deformed to match.
struct Posed {
  Posed(const Character& c, const std::map<std::string, double>& turns);
  const Character& base;
  std::vector<Mat3> world_R;
  std::vector<Vec3> world_T;
  std::vector<MeshData> meshes(bool skip_weapons) const;
};
std::map<std::string, double> fist_turns(const std::vector<std::string>& names);
bool is_weapon_group(const std::string& group);

// Places a fighter's body on a Real Steel movie robot's skeleton (its own bone
// frames and joint positions, so the robot's animations play on it).
struct Retarget {
  Retarget(const Posed& jaeger, const Character& base, double height);
  const Posed& J;
  const Character& B;
  double s = 1;                      // fighter units -> Real Steel
  std::vector<int> anchor;           // per RS bone: fighter bone or -1
  std::vector<Mat3> R;               // per RS bone: turn of the fighter geometry
  std::vector<Vec3> Jt;              // per RS bone: joint (world)
  std::vector<int> rs_bone;          // per fighter bone: RS bone that carries it
  std::vector<MeshData> meshes;      // the fighter's (posed) meshes, weapons left out
  Vec3 place(int b, const Vec3& p) const;
};
std::string fighter_bone_name(const std::string& rs_name, const std::vector<std::string>& names);

// The converted model (JBOY bytes), the texture packs and the character entry
// (the base robot's entry with items 0, 0x0A, 0x0C replaced).
Bytes build_model(const Retarget& rt, const std::vector<std::vector<uint32_t>>& colours);
std::pair<Bytes, Bytes> build_textures(const Character& fighter, const Character& base);
Bytes character_entry(const Bytes& base_entry_raw, const Bytes& model, const Bytes& item_a, const Bytes& item_c);
// raw PACH bytes of an entry of a character pack
Bytes pack_entry(const Bytes& pack, const std::string& name);
// every vertex of the meshes (for sizes)
std::vector<Vec3> body_points(const std::vector<MeshData>& meshes);

}  // namespace rs::primport
