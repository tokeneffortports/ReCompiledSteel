// Fighter import: bodies (see primport_model.h). The conversion keeps the
// limits the game's renderer has: at most two triangle strips per mesh (of
// up to 4000 triangles each), under 15000 vertices and 20 palette bones a
// mesh, 3 weights a vertex; vertices in runs by bone combination.
#include "primport_model.h"

#include <algorithm>
#include <cstring>
#include <set>

namespace rs::primport {

namespace {
constexpr int kMaxPalette = 20;
constexpr int kStripTriangles = 4000;
constexpr int kMaxStrips = 2;
constexpr int kMaxWeights = 3;
constexpr size_t kMaxVertices = 15000;
constexpr double kSphereMargin = 1.6;

const char* const kWeaponGroups[] = {"sword_a", "sword_b", "gun_a", "gun_b", "l_arm_parts", "ct_l_arm_parts", "ct_r_arm_parts"};

// Real Steel bone -> the fighter's bone at the same joint (L_/R_ for l_/r_)
const std::map<std::string, std::string> kAnchors = {
    {"root", "root"},      {"koshi", "Spine"}, {"mune", "Spine2"}, {"kubi", "Neck"},    {"atama", "Head"},
    {"sakotsu", "Clavicle"}, {"ninoude", "UpperArm"}, {"kote", "Forearm"}, {"te", "Hand"},
    {"momo", "Thigh"},     {"sune", "Calf"},   {"ashi", "Foot"},   {"tsumasaki", "Toe0"}};
// limb bones turned onto the base robot's limb direction: bone -> (child that
// sets the direction, bone whose turn it shares)
struct Limb {
  const char* child;
  const char* share;
};
const std::map<std::string, Limb> kLimbs = {{"ninoude", {"kote", nullptr}}, {"kote", {"te", nullptr}},  {"te", {nullptr, "kote"}},
                                            {"momo", {"sune", nullptr}},    {"sune", {"ashi", nullptr}}, {"ashi", {nullptr, "sune"}},
                                            {"tsumasaki", {nullptr, "sune"}}};
const std::set<std::string> kArm = {"sakotsu", "ninoude", "kote", "te"};

std::pair<char, std::string> side_name(const std::string& n) {
  if (n.size() > 2 && (n[0] == 'l' || n[0] == 'r') && n[1] == '_') return {n[0], n.substr(2)};
  return {0, n};
}

Bytes entry_items_source(const Bytes& raw) { return is_bpe(raw) ? bpe_decode(raw.data(), raw.size()) : raw; }
}  // namespace

bool is_weapon_group(const std::string& g) {
  for (const char* w : kWeaponGroups)
    if (g == w) return true;
  return false;
}

Bytes pack_entry(const Bytes& pack, const std::string& name) {
  Archive a(pack);
  for (const auto& e : a.entries())
    if (name.empty() || e.name == name) return entry_items_source(e.raw);
  throw Error("character pack has no entry " + name);
}

Character::Character(const Bytes& pack, const std::string& entry) {
  for (const PachItem& it : pach_items(pack_entry(pack, entry))) items[it.id] = it.data();
  if (!items.count(0)) throw Error("character without a model");
  model = std::make_unique<Model>(items[0]);
  for (const Bone& b : model->bones) {
    const int i = int(names.size());
    names.push_back(b.name);
    parent.push_back(b.parent);
    local_R.push_back(euler(b.rot[0], b.rot[1], b.rot[2]));
    local_pos.push_back(b.pos);
    const int p = b.parent;
    if (p >= 0 && p < i) {
      world_R.push_back(world_R[p] * local_R[i]);
      world_T.push_back(world_T[p] + world_R[p] * b.pos);
    } else {
      world_R.push_back(local_R[i]);
      world_T.push_back(b.pos);
    }
  }
  for (uint32_t id : {0x0Au, 0x0Cu}) {
    if (!items.count(id)) continue;
    for (auto& [name, dds] : texpack_read(items[id])) textures[name] = dds;
  }
}

int Character::bone(const std::string& name) const {
  for (size_t i = 0; i < names.size(); ++i)
    if (names[i] == name) return int(i);
  return -1;
}

std::vector<MeshData> Character::meshes(bool skip_weapons) const {
  std::vector<MeshData> out;
  for (const Mesh& m : model->meshes) {
    MeshData d;
    d.group = m.group < model->group_names.size() ? model->group_names[m.group] : "?";
    if (skip_weapons && is_weapon_group(d.group)) continue;
    std::vector<std::array<double, 6>> v;
    model->vertices(m, &v, &d.colour);
    for (const auto& x : v) {
      d.pos.push_back({x[0], x[1], x[2]});
      d.nrm.push_back({x[3], x[4], x[5]});
    }
    d.uv = model->uvs(m);
    d.skin = model->skin(m);
    d.tris = model->triangles(m);
    out.push_back(std::move(d));
  }
  return out;
}

std::map<std::string, double> fist_turns(const std::vector<std::string>& names) {
  std::map<std::string, double> t;
  for (const std::string& n : names) {
    std::string b = (n.size() > 2 && (n.compare(0, 2, "L_") == 0 || n.compare(0, 2, "R_") == 0)) ? n.substr(2) : n;
    if (b.size() != 8 || b.compare(0, 6, "Finger") != 0) continue;
    const char digit = b[6], joint = b[7];
    const double* table;
    static const double thumb[3] = {-0.35, -0.6, -0.4}, finger[3] = {-1.35, -1.45, -1.0};
    table = digit == '0' ? thumb : finger;
    t[n] = (joint >= '1' && joint <= '3') ? table[joint - '1'] : 0.0;
  }
  return t;
}

Posed::Posed(const Character& c, const std::map<std::string, double>& turns) : base(c) {
  for (size_t i = 0; i < c.names.size(); ++i) {
    const auto it = turns.find(c.names[i]);
    const Mat3 local = c.local_R[i] * rot_y(it == turns.end() ? 0.0 : it->second);
    const int p = c.parent[i];
    if (p >= 0 && p < int(i)) {
      world_R.push_back(world_R[p] * local);
      world_T.push_back(world_T[p] + world_R[p] * c.local_pos[i]);
    } else {
      world_R.push_back(local);
      world_T.push_back(c.local_pos[i]);
    }
  }
}

std::vector<MeshData> Posed::meshes(bool skip_weapons) const {
  std::vector<MeshData> out = base.meshes(skip_weapons);
  std::vector<Mat3> M;
  for (size_t i = 0; i < world_R.size(); ++i) M.push_back(world_R[i] * transpose(base.world_R[i]));
  for (MeshData& d : out) {
    for (size_t v = 0; v < d.pos.size(); ++v) {
      Vec3 p{0, 0, 0}, n{0, 0, 0};
      for (const auto& [b, w] : d.skin[v]) {
        p = p + (M[b] * (d.pos[v] - base.world_T[b]) + world_T[b]) * w;
        n = n + (M[b] * d.nrm[v]) * w;
      }
      d.pos[v] = p;
      d.nrm[v] = n * (1.0 / std::max(norm(n), 1e-9));
    }
  }
  return out;
}

std::vector<Vec3> body_points(const std::vector<MeshData>& meshes) {
  std::vector<Vec3> out;
  for (const MeshData& m : meshes) out.insert(out.end(), m.pos.begin(), m.pos.end());
  return out;
}

std::string fighter_bone_name(const std::string& rs_name, const std::vector<std::string>& names) {
  const auto [side, base] = side_name(rs_name);
  const auto a = kAnchors.find(base);
  if (a == kAnchors.end()) return "";
  const std::string& name = a->second;
  auto has = [&](const std::string& n) { return std::find(names.begin(), names.end(), n) != names.end(); };
  if (!side) return has(name) ? name : "";
  const std::string p = side == 'l' ? "L_" : "R_";
  std::string trimmed = name;
  while (!trimmed.empty() && trimmed.back() == '0') trimmed.pop_back();
  for (const std::string& c : {p + name, p + name + "_01", p + "01_" + name, p + "01_" + name + "_01", p + trimmed})
    if (has(c)) return c;
  return "";
}

Retarget::Retarget(const Posed& jaeger, const Character& base, double height) : J(jaeger), B(base) {
  meshes = jaeger.meshes(true);
  double lo = 1e30, hi = -1e30;
  for (const MeshData& m : meshes)
    for (const Vec3& p : m.pos) lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
  s = height / (hi - lo);
  const size_t n = B.names.size();
  anchor.assign(n, -1);
  for (size_t i = 0; i < n; ++i) {
    const std::string jn = fighter_bone_name(B.names[i], J.base.names);
    if (!jn.empty()) anchor[i] = J.base.bone(jn);
  }
  R.assign(n, Mat3::identity());
  Jt = B.world_T;    // the base robot's own joints (its animations are made for them)
  auto index_of = [&](const std::string& name) {
    const int i = B.bone(name);
    if (i < 0) throw Error("base robot has no bone " + name);
    return i;
  };
  for (size_t i = 0; i < n; ++i) {
    const auto [side, bn] = side_name(B.names[i]);
    const auto l = kLimbs.find(bn);
    if (l == kLimbs.end() || anchor[i] < 0 || !l->second.child) continue;
    const int c = index_of(B.names[i].substr(0, 2) + l->second.child);
    if (anchor[c] < 0) continue;
    const Vec3 src = J.world_T[anchor[c]] - J.world_T[anchor[i]];
    R[i] = rotation_between(src, B.world_T[c] - B.world_T[i]);
  }
  for (size_t i = 0; i < n; ++i) {
    const auto [side, bn] = side_name(B.names[i]);
    const auto l = kLimbs.find(bn);
    if (l != kLimbs.end() && l->second.share) R[i] = R[index_of(B.names[i].substr(0, 2) + l->second.share)];
  }
  // every fighter bone -> the RS bone of its nearest mapped ancestor
  std::map<int, int> rs_of_anchor;
  for (size_t i = 0; i < n; ++i)
    if (anchor[i] >= 0) rs_of_anchor[anchor[i]] = int(i);
  for (size_t j = 0; j < J.base.names.size(); ++j) {
    int k = int(j);
    while (k >= 0 && !rs_of_anchor.count(k)) k = J.base.parent[k];
    rs_bone.push_back(k >= 0 ? rs_of_anchor[k] : 0);
  }
}

Vec3 Retarget::place(int b, const Vec3& p) const {
  const int a = anchor[b];
  if (a < 0) return p * s;
  return Jt[b] + R[b] * ((p - J.world_T[a]) * s);
}

// --- the model ---------------------------------------------------------------
namespace {

struct Spec {
  const Mesh* tmpl = nullptr;
  std::vector<uint32_t> bones;
  int weights = 1;
  std::vector<std::array<double, 6>> vertices;
  std::vector<uint32_t> colour;
  std::vector<std::vector<std::pair<uint32_t, float>>> vw;
  std::vector<std::array<double, 2>> uvs;
  std::vector<std::vector<uint16_t>> strips;
};

Bytes encode_pof0(std::vector<uint32_t> pos) {
  std::sort(pos.begin(), pos.end());
  Bytes body;
  uint32_t last = 0;
  for (uint32_t p : pos) {
    const uint32_t d = (p - last) / 4;
    if (d < 0x40) {
      body.push_back(uint8_t(0x40 | d));
    } else if (d < 0x4000) {
      body.push_back(uint8_t(0x80 | (d >> 8)));
      body.push_back(uint8_t(d));
    } else {
      body.insert(body.end(), {uint8_t(0xC0 | (d >> 24)), uint8_t(d >> 16), uint8_t(d >> 8), uint8_t(d)});
    }
    last = p;
  }
  while (body.size() % 4) body.push_back(0);
  Bytes out = {'P', 'O', 'F', '0'};
  app_be32(out, uint32_t(body.size()));
  app(out, body);
  return out;
}

Bytes write_jboy(const Model& t, const std::vector<Spec>& specs, const Bytes& bone_table) {
  constexpr size_t kHeader = 0x40, kMesh = 0xB4;
  const Bytes& td = t.data;
  Bytes out(kHeader + kMesh * specs.size(), 0);
  std::vector<uint32_t> ptrs = {0x14, 0x20, 0x24, 0x28};
  std::memcpy(out.data(), td.data() + 8, kHeader);
  put_be32(&out[0x10], 0x80000000u | uint32_t(specs.size()));
  put_be32(&out[0x14], uint32_t(kHeader));
  for (size_t i = 0; i < specs.size(); ++i) {
    const Spec& sp = specs[i];
    const Mesh& tm = *sp.tmpl;
    const size_t mo = kHeader + i * kMesh;
    std::memcpy(&out[mo], td.data() + tm.offset, kMesh);
    const size_t n = sp.vertices.size();
    if (n >= 65536 || sp.bones.empty() || sp.bones.size() > 20) throw Error("mesh outside the game's limits");
    put_be32(&out[mo], uint32_t(n));
    put_be32(&out[mo + 4], uint32_t(sp.strips.size()));
    put_be32(&out[mo + 8], uint32_t(sp.bones.size()));
    for (size_t k = 0; k < 20; ++k) put_be32(&out[mo + 0x0C + 4 * k], k < sp.bones.size() ? sp.bones[k] : 0xFFFFFFFFu);
    put_be32(&out[mo + 0x5C], uint32_t(sp.weights));
    put_be32(&out[mo + 0x74], tm.group74);

    const uint32_t param_table = uint32_t(out.size());
    out.insert(out.end(), td.begin() + 8 + tm.param_table, td.begin() + 8 + tm.vertex_block);
    const int64_t delta = int64_t(param_table) - int64_t(tm.param_table);
    for (uint32_t k = 0; k < tm.param_count; ++k) {
      const uint32_t p = uint32_t(int64_t(be32(td.data() + 8 + tm.param_table + k * 4)) + delta);
      put_be32(&out[param_table + k * 4], p);
      ptrs.push_back(param_table + k * 4);
    }
    put_be32(&out[mo + 0x94], param_table);

    const uint32_t vertex_block = uint32_t(out.size());
    app_be32(out, vertex_block + 4);
    ptrs.push_back(vertex_block);
    for (size_t v = 0; v < n; ++v) {
      for (int k = 0; k < 6; ++k) app_bef(out, sp.vertices[v][k]);
      app_be32(out, sp.colour[v]);
    }
    put_be32(&out[mo + 0x68], vertex_block);

    const uint32_t weight_block = uint32_t(out.size());
    for (const auto& entry : sp.vw)
      for (const auto& [slot, w] : entry) {
        app_be32(out, slot);
        app_bef(out, w);
      }
    put_be32(&out[mo + 0x6C], weight_block);

    const uint32_t uv_block = uint32_t(out.size());
    for (const auto& uv : sp.uvs) {
      app_bef(out, uv[0]);
      app_bef(out, uv[1]);
    }
    put_be32(&out[mo + 0x70], uv_block);

    const uint32_t strip_table = uint32_t(out.size());
    out.resize(out.size() + 12 * sp.strips.size(), 0);
    for (size_t k = 0; k < sp.strips.size(); ++k) {
      put_be32(&out[strip_table + 12 * k], 6);
      put_be32(&out[strip_table + 12 * k + 4], uint32_t(sp.strips[k].size()));
      put_be32(&out[strip_table + 12 * k + 8], uint32_t(out.size()));
      ptrs.push_back(uint32_t(strip_table + 12 * k + 8));
      for (uint16_t ix : sp.strips[k]) app_be16(out, ix);
      while (out.size() % 4) out.push_back(0);
    }
    put_be32(&out[mo + 0x98], strip_table);
    put_be32(&out[mo + 0x9C], uint32_t(n));
    // bounding sphere: the box centre, the furthest vertex, with room for the animation
    Vec3 lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const auto& v : sp.vertices)
      for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
    const Vec3 c = (lo + hi) * 0.5;
    double r = 0;
    for (const auto& v : sp.vertices) r = std::max(r, norm(Vec3{v[0], v[1], v[2]} - c));
    if (sp.vertices.empty()) r = 0.05;
    put_bef(&out[mo + 0xA4], c[0]);
    put_bef(&out[mo + 0xA8], c[1]);
    put_bef(&out[mo + 0xAC], c[2]);
    put_bef(&out[mo + 0xB0], r * kSphereMargin);
    for (uint32_t off : {0x68u, 0x6Cu, 0x70u, 0x94u, 0x98u}) ptrs.push_back(uint32_t(mo + off));
  }
  while (out.size() % 16) out.push_back(0);
  put_be32(&out[0x20], uint32_t(out.size()));
  app(out, bone_table);
  put_be32(&out[0x24], uint32_t(out.size()));
  out.insert(out.end(), td.begin() + 8 + t.texture_table, td.begin() + 8 + t.texture_table + 16 * t.texture_count);
  put_be32(&out[0x28], uint32_t(out.size()));
  out.insert(out.end(), td.begin() + 8 + t.group_table, td.begin() + 8 + t.group_table + 32 * t.group_count);
  const uint32_t fixups = uint32_t(out.size());
  app(out, encode_pof0(ptrs));
  Bytes data(td.begin(), td.begin() + 8);
  app(data, out);
  put_be32(&data[4], fixups);
  put_be32(&data[0x0C], fixups);
  return data;
}

}  // namespace

Bytes build_model(const Retarget& rt, const std::vector<std::vector<uint32_t>>& colours) {
  const Character& B = rt.B;
  const Model& base = *B.model;
  const auto& groups = base.group_names;
  auto gindex = [&](const std::string& g) {
    for (size_t i = 0; i < groups.size(); ++i)
      if (groups[i] == g) return int(i);
    return -1;
  };
  const int arm_group = std::max(0, gindex("arm"));
  int body_group = 0;
  for (size_t i = 0; i < groups.size(); ++i) {
    const std::string& g = groups[i];
    if (g != "arm" && g != "cy" && g != "inner" && g != "glow" && g != "shoulder" && g != "01") {
      body_group = int(i);
      break;
    }
  }
  const int cy_group = gindex("cy");
  int hip_group = body_group;
  for (size_t i = 0; i < groups.size(); ++i)
    if (int(i) != arm_group && int(i) != body_group && int(i) != cy_group) {
      hip_group = int(i);
      break;
    }
  std::map<uint32_t, const Mesh*> template_of;
  for (const Mesh& m : base.meshes) template_of.emplace(m.group, &m);

  // vertices on the RS skeleton
  struct Conv {
    std::vector<Vec3> p, n;
    std::vector<std::vector<std::pair<int, double>>> w;
  };
  std::vector<Conv> conv;
  for (const MeshData& m : rt.meshes) {
    Conv c;
    for (size_t v = 0; v < m.pos.size(); ++v) {
      std::vector<std::pair<int, double>> acc;
      for (const auto& [jb, w] : m.skin[v]) {
        const int b = rt.rs_bone[jb];
        auto it = std::find_if(acc.begin(), acc.end(), [&](const auto& x) { return x.first == b; });
        if (it == acc.end()) acc.emplace_back(b, w);
        else it->second += w;
      }
      std::stable_sort(acc.begin(), acc.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
      if (acc.size() > size_t(kMaxWeights)) acc.resize(kMaxWeights);
      double total = 0;
      for (const auto& x : acc) total += x.second;
      if (total == 0) total = 1;
      for (auto& x : acc) x.second /= total;
      Vec3 p{0, 0, 0}, n{0, 0, 0};
      for (const auto& [b, w] : acc) {
        p = p + rt.place(b, m.pos[v]) * w;
        n = n + (rt.R[b] * m.nrm[v]) * w;
      }
      c.p.push_back(p);
      c.n.push_back(n * (1.0 / std::max(norm(n), 1e-9)));
      c.w.push_back(acc);
    }
    conv.push_back(std::move(c));
  }

  // triangles into meshes, per group
  struct Bucket {
    std::set<int> bones;
    std::vector<std::array<int, 4>> tris;    // (mesh, a, b, c)
    std::set<std::pair<int, int>> verts;
  };
  std::map<int, std::vector<Bucket>> buckets;
  for (size_t mi = 0; mi < rt.meshes.size(); ++mi) {
    const MeshData& m = rt.meshes[mi];
    const Conv& c = conv[mi];
    for (const auto& t : m.tris) {
      std::set<int> bones;
      std::vector<std::pair<int, double>> weight;
      for (int v : t)
        for (const auto& [x, ww] : c.w[v]) {
          bones.insert(x);
          auto it = std::find_if(weight.begin(), weight.end(), [&](const auto& y) { return y.first == x; });
          if (it == weight.end()) weight.emplace_back(x, ww);
          else it->second += ww;
        }
      int dom = weight[0].first;
      double best = weight[0].second;
      for (const auto& [x, ww] : weight)
        if (ww > best) best = ww, dom = x;
      int dom_j = -1;
      double bj = -1;
      for (int v : t)
        for (const auto& [jb, ww] : m.skin[v])
          if (ww > bj) bj = ww, dom_j = jb;
      const std::string jname = dom_j >= 0 ? rt.J.base.names[dom_j] : "";
      int g;
      if (cy_group >= 0 && jname.compare(0, 2, "cy") == 0) g = cy_group;
      else if (kArm.count(side_name(B.names[dom]).second)) g = arm_group;
      else if (B.names[dom] == "root" || B.names[dom] == "koshi") g = hip_group;
      else g = body_group;
      std::vector<Bucket>& lst = buckets[g];
      Bucket* target = nullptr;
      for (Bucket& b : lst) {
        std::set<int> u = b.bones;
        u.insert(bones.begin(), bones.end());
        if (int(u.size()) <= kMaxPalette && b.verts.size() < kMaxVertices && b.tris.size() < size_t(kMaxStrips * kStripTriangles)) {
          b.bones = std::move(u);
          target = &b;
          break;
        }
      }
      if (!target) {
        lst.push_back(Bucket{bones, {}, {}});
        target = &lst.back();
      }
      target->tris.push_back({int(mi), t[0], t[1], t[2]});
      for (int v : t) target->verts.insert({int(mi), v});
    }
  }

  std::vector<Spec> specs;
  std::vector<int> counts;
  for (int g = 0; g < int(groups.size()); ++g) {
    int here = 0;
    for (Bucket& bk : buckets[g]) {
      Spec sp;
      const auto ti = template_of.find(uint32_t(g));
      sp.tmpl = ti != template_of.end() ? ti->second : &base.meshes[0];
      const std::vector<int> palette(bk.bones.begin(), bk.bones.end());
      for (int b : palette) sp.bones.push_back(uint32_t(b + 1));
      int weights = 1;
      for (const auto& [mi, v] : bk.verts) weights = std::max(weights, int(conv[mi].w[v].size()));
      sp.weights = weights;
      std::map<std::pair<int, int>, int> index;
      std::vector<std::array<int, 3>> tris;
      for (const auto& t : bk.tris) {
        std::array<int, 3> ids{};
        for (int k = 0; k < 3; ++k) {
          const std::pair<int, int> key{t[0], t[1 + k]};
          auto it = index.find(key);
          if (it == index.end()) {
            const int mi = t[0], v = t[1 + k];
            it = index.emplace(key, int(sp.vertices.size())).first;
            const Conv& c = conv[mi];
            sp.vertices.push_back({c.p[v][0], c.p[v][1], c.p[v][2], c.n[v][0], c.n[v][1], c.n[v][2]});
            sp.colour.push_back(colours[mi][v]);
            std::vector<std::pair<uint32_t, float>> entry;
            for (const auto& [bb, ww] : c.w[v]) {
              const uint32_t slot = uint32_t(std::find(palette.begin(), palette.end(), bb) - palette.begin());
              entry.emplace_back(slot << 24, float(ww));
            }
            while (int(entry.size()) < weights) entry.emplace_back(0xFFu << 24, 0.0f);
            sp.vw.push_back(entry);
            sp.uvs.push_back(rt.meshes[mi].uv[v]);
          }
          ids[k] = it->second;
        }
        tris.push_back(ids);
      }
      // vertices in runs by bone combination (one-bone vertices first)
      std::vector<int> order(sp.vertices.size());
      for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
      auto real = [&](int i) {
        std::vector<uint32_t> r;
        for (const auto& [slot, w] : sp.vw[i])
          if ((slot >> 24) != 0xFF) r.push_back(slot >> 24);
        return r;
      };
      std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const auto ra = real(a), rb = real(b);
        if (ra.size() != rb.size()) return ra.size() < rb.size();
        if (ra != rb) return ra < rb;
        return a < b;
      });
      std::vector<int> new_index(order.size());
      for (size_t n = 0; n < order.size(); ++n) new_index[order[n]] = int(n);
      Spec sorted = sp;
      for (size_t n = 0; n < order.size(); ++n) {
        sorted.vertices[n] = sp.vertices[order[n]];
        sorted.colour[n] = sp.colour[order[n]];
        sorted.vw[n] = sp.vw[order[n]];
        sorted.uvs[n] = sp.uvs[order[n]];
      }
      for (auto& t : tris)
        for (int& v : t) v = new_index[v];
      for (size_t k = 0; k < tris.size(); k += kStripTriangles) {
        std::vector<uint16_t> s;
        for (size_t j = k; j < std::min(tris.size(), k + kStripTriangles); ++j) {
          const auto& t = tris[j];
          s.insert(s.end(), {uint16_t(t[1]), uint16_t(t[1]), uint16_t(t[0]), uint16_t(t[2]), uint16_t(t[2]), uint16_t(t[2])});
        }
        sorted.strips.push_back(std::move(s));
      }
      specs.push_back(std::move(sorted));
      ++here;
    }
    counts.push_back(here);
  }
  // bone table: the base frames at the new joint positions
  Bytes bt;
  for (size_t i = 0; i < base.bones.size(); ++i) {
    Bytes rec(base.bones[i].raw.begin(), base.bones[i].raw.end());
    const int p = B.parent[i];
    const Vec3 lp = p < 0 ? rt.Jt[i] : transpose(B.world_R[p]) * (rt.Jt[i] - rt.Jt[p]);
    put_bef(&rec[16], lp[0]);
    put_bef(&rec[20], lp[1]);
    put_bef(&rec[24], lp[2]);
    app(bt, rec);
  }
  Bytes data = write_jboy(base, specs, bt);
  const uint32_t group_table = be32(&data[0x30]);
  for (size_t g = 0; g < counts.size(); ++g) put_be32(&data[8 + group_table + g * 32 + 24], uint32_t(counts[g]));
  return data;
}

// --- textures ----------------------------------------------------------------
namespace {
double gloss_map(double a) { return a * 3 + 30; }   // the fighters' gloss (mean 8) lifted towards the stock robots' (~60)
}

std::pair<Bytes, Bytes> build_textures(const Character& fighter, const Character& base) {
  auto need = [&](const std::string& n) -> const Bytes& {
    const auto it = fighter.textures.find(n);
    if (it == fighter.textures.end()) throw Error("fighter has no " + n + " texture");
    return it->second;
  };
  const auto base_a = texpack_read(base.items.at(0x0A)), base_c = texpack_read(base.items.at(0x0C));
  auto base_tex = [&](const std::vector<std::pair<std::string, Bytes>>& t, const std::string& n) -> const Bytes& {
    for (const auto& [name, d] : t)
      if (name == n) return d;
    throw Error("base robot has no " + n + " texture");
  };
  const Bytes colour = dxt5_from(need("color"), need("normal"), gloss_map);
  const Bytes normal = dxt5_remap_alpha(dds_drop_top(need("normal"), 1), gloss_map);
  // glow: the fighter's glow mask (any colour) -> the red channel the game reads
  const Image g = dds_decode(need("glowmask"));
  const Bytes& base_glow = base_tex(base_a, "glowmask");
  const DdsInfo gi = dds_info(base_glow);
  const int fx = g.w / gi.w, fy = g.h / gi.h;
  if (fx < 1 || fy < 1) throw Error("glow mask smaller than the base robot's");
  std::vector<double> red(size_t(gi.w) * gi.h, 0.0);
  for (int y = 0; y < gi.h; ++y)
    for (int x = 0; x < gi.w; ++x) {
      double sum = 0;
      for (int dy = 0; dy < fy; ++dy)
        for (int dx = 0; dx < fx; ++dx) {
          const uint8_t* p = g.at(x * fx + dx, y * fy + dy);
          sum += std::max({p[0], p[1], p[2]});
        }
      red[size_t(y) * gi.w + x] = sum / (fx * fy);
    }
  const Bytes glow = encode_red_dxt5(red, gi.w, gi.h, base_glow);
  const Bytes& base_mk = base_tex(base_a, "Mk");
  const DdsInfo mi = dds_info(base_mk);
  const Bytes mk = constant_dxt1(mi.w, mi.h, {200, 200, 200}, base_mk);
  std::vector<std::pair<std::string, Bytes>> a, c;
  for (const auto& [name, d] : base_a) a.emplace_back(name, name == "normal" ? normal : name == "glowmask" ? glow : name == "Mk" ? mk : d);
  for (const auto& [name, d] : base_c) c.emplace_back(name, name == "color" ? colour : d);
  return {texpack_write(a), texpack_write(c)};
}

Bytes character_entry(const Bytes& base_entry_raw, const Bytes& model, const Bytes& item_a, const Bytes& item_c) {
  std::vector<PachItem> items = pach_items(base_entry_raw);
  for (PachItem& it : items) {
    const Bytes* nw = it.id == 0 ? &model : it.id == 0x0A ? &item_a : it.id == 0x0C ? &item_c : nullptr;
    if (nw) it.raw = is_bpe(it.raw) ? bpe_store(*nw) : *nw;
  }
  return pach_build(items);
}

}  // namespace rs::primport
