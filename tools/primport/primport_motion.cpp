// Fighter import: motions (see primport_motion.h). The decoding follows the
// game (Real Steel's sub_8209F5E8: 5-byte keys are an axis given as two
// 16-bit angles plus a 16-bit angle; rate classes space the keys).
#include "primport_motion.h"

#include <algorithm>
#include <cstring>

namespace rs::primport {

namespace {
constexpr double kPi = 3.14159265358979323846;
const double kRateStep[6] = {1.0, 15 / 11.0, 1.875, 30 / 11.0, 3.75, 5.0};
size_t rot_size(int t) { return t == 0 ? 4 : t == 2 ? 6 : 5; }
size_t trans_size(int t) { return t == 1 ? 6 : t == 2 ? 12 : 0; }
size_t scale_size(int t) { return t == 1 ? 6 : (t == 2 || t == 3) ? 12 : 0; }

int num_keys(int rclass, int frames) {
  if (rclass == 0) return frames;
  const double f = std::max(frames - 1.0, 0.0) / kRateStep[rclass];
  int n = int(f);
  if (f - n >= 1e-4) ++n;
  return n + 1;
}

double half(uint16_t h) {
  const double s = (h & 0x8000) ? -1.0 : 1.0;
  const int e = (h >> 10) & 0x1F, m = h & 0x3FF;
  if (e == 0 && m == 0) return 0.0 * s;
  return s * (1.0 + m / 1024.0) * std::pow(2.0, e - 15);
}

double sin16(int a) { return std::sin((a & 0xFFFF) * (2 * kPi / 65536.0)); }
double cos16(int a) { return std::cos((a & 0xFFFF) * (2 * kPi / 65536.0)); }

Quat unpack_rot(int type, const uint8_t* b) {
  int lon, lat, ang;
  if (type == 0) {
    const uint32_t v = b[0] | (b[1] << 8) | (b[2] << 16) | (uint32_t(b[3]) << 24);
    lon = int((v >> 22) & 0x3FF) << 6;
    lat = int((v >> 12) & 0x3FF) << 5;
    ang = int(v & 0xFFF) << 4;
  } else if (type == 1) {
    lon = (((((b[1] & 0x0F) << 8) | b[0]) << 2) | (b[3] & 3)) << 2;
    lat = (((((b[1] & 0xF0) << 4) | b[2]) << 1) | ((b[3] >> 2) & 1)) << 2;
    ang = ((b[4] << 8) | b[3]) & 0xFFF8;
  } else if (type == 2) {
    lon = (b[1] << 8) | b[0];
    lat = (b[3] << 8) | b[2];
    ang = (b[5] << 8) | b[4];
  } else {
    lon = (((((b[1] & 0x0F) << 8) | b[0]) << 1) | (b[3] & 1)) << 3;
    lat = (((((b[1] & 0xF0) << 4) | b[2]) << 1) | ((b[3] >> 1) & 1)) << 2;
    ang = ((b[4] << 8) | b[3]) & 0xFFFC;
  }
  lon &= 0xFFFF;
  lat &= 0xFFFF;
  if (ang >= 0x8000) ang -= 0x10000;
  const double sl = sin16(lat);
  const double ax = cos16(lat), ay = sl * sin16(lon), az = -sl * cos16(lon);
  const double a = ang * (kPi / 32768.0), s = std::sin(a * 0.5);
  return {ax * s, ay * s, az * s, std::cos(a * 0.5)};
}

// The 5-byte key (type 1) of a rotation.
void pack_rot(const Quat& q, Bytes& out) {
  const double w = std::clamp(q[3], -1.0, 1.0);
  double a = 2 * std::acos(w);
  const double s = std::sqrt(std::max(0.0, 1 - w * w));
  Vec3 ax{1, 0, 0};
  if (s < 1e-6) {
    a = 0;
  } else {
    ax = {q[0] / s, q[1] / s, q[2] / s};
  }
  const double lat = std::acos(std::clamp(ax[0], -1.0, 1.0));
  const double lon = std::abs(std::sin(lat)) > 1e-9 ? std::atan2(ax[1], -ax[2]) : 0.0;
  double lm = std::fmod(lon, 2 * kPi);
  if (lm < 0) lm += 2 * kPi;
  uint32_t L = uint32_t(int64_t(std::nearbyint(lm / (2 * kPi) * 65536))) & 0xFFFF;
  uint32_t T = uint32_t(std::min<int64_t>(int64_t(std::nearbyint(lat / kPi * 32768)), 32767));
  const uint32_t A = uint32_t(std::min<int64_t>(int64_t(std::nearbyint(a / kPi * 32768)), 32767)) & 0xFFF8;
  L >>= 2;
  T >>= 2;
  out.push_back(uint8_t((L >> 2) & 0xFF));
  out.push_back(uint8_t(((L >> 10) & 0xF) | (((T >> 9) & 0xF) << 4)));
  out.push_back(uint8_t((T >> 1) & 0xFF));
  out.push_back(uint8_t((A & 0xF8) | ((T & 1) << 2) | (L & 3)));
  out.push_back(uint8_t((A >> 8) & 0xFF));
}

std::vector<int> twin_index(const Character& B) {
  std::vector<int> out;
  for (const std::string& n : B.names) {
    std::string t = n;
    if (n.compare(0, 2, "l_") == 0) t = "r_" + n.substr(2);
    else if (n.compare(0, 2, "r_") == 0) t = "l_" + n.substr(2);
    const int i = B.bone(t);
    out.push_back(i >= 0 ? i : B.bone(n));
  }
  return out;
}

std::vector<Vec3> rs_world(const Character& B, const Pose& p) {
  std::vector<Mat3> R(B.names.size());
  std::vector<Vec3> T(B.names.size());
  for (size_t i = 0; i < B.names.size(); ++i) {
    const auto it = p.local.find(int(i));
    const Mat3& lr = it != p.local.end() ? it->second : B.local_R[i];
    const int par = B.parent[i];
    if (par < 0) {
      R[i] = lr;
      T[i] = p.root;
    } else {
      R[i] = R[par] * lr;
      T[i] = T[par] + R[par] * B.local_pos[i];
    }
  }
  return T;
}
}  // namespace

MotionBank::MotionBank(Bytes data) : d_(std::move(data)) {
  const Bytes& d = d_;
  size_t n = 0;
  while (n * 16 < d.size() && d[n * 16] != 0) ++n;
  ns_ = 0x1000;
  while (ns_ < n * 16) ns_ <<= 1;
  for (size_t i = 0; i < ns_ / 16 && i * 16 < d.size(); ++i) {
    const char* s = reinterpret_cast<const char*>(&d[i * 16]);
    if (!s[0]) break;
    bones_.emplace_back(s, strnlen(s, 16));
  }
  if (d.size() < ns_ + 4) throw Error("motion bank too short");
  const size_t b = ns_;
  const uint32_t dir_off = be16(&d[b]), max_sets = be16(&d[b + 2]);
  const size_t desc_base = b + 4 + size_t(max_sets) * 4;
  const size_t db = b + dir_off;
  const uint32_t count = be32(&d[db]);
  const size_t dir_base = db + 4, data_base = dir_base + size_t(count) * 32;
  if (data_base > d.size()) throw Error("motion bank directory past its end");
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = &d[dir_base + i * 32];
    Motion m;
    m.key = be32(e);
    m.act = int(m.key >> 16), m.sub = int((m.key >> 8) & 0xFF), m.track = int(m.key & 0xFF);
    const uint32_t offset = be32(e + 4);
    m.frames = be16(e + 8);
    m.set = e[10];
    if (m.set >= int(max_sets)) throw Error("motion with an unknown track set");
    const uint16_t cnt = be16(&d[b + 4 + m.set * 4]), start = be16(&d[b + 6 + m.set * 4]);
    size_t pos = data_base + offset;
    for (uint32_t k = 0; k < cnt; ++k) {
      const uint8_t* q = &d[desc_base + (size_t(start) + k) * 4];
      Track t;
      t.bone = be16(q);
      t.name = t.bone < int(bones_.size()) ? bones_[t.bone] : "bone" + std::to_string(t.bone);
      t.rclass = q[2] & 0xF, t.rtype = q[2] >> 4, t.ttype = q[3] & 0xF, t.stype = q[3] >> 4;
      if (t.rclass > 5) throw Error("motion with an unknown key rate");
      t.nkeys = num_keys(t.rclass, m.frames);
      t.toff = pos;
      pos += trans_size(t.ttype) * t.nkeys;
      t.roff = pos;
      pos += rot_size(t.rtype) * t.nkeys;
      pos += scale_size(t.stype) * t.nkeys;
      m.tracks.push_back(t);
    }
    if (pos > d.size()) throw Error("motion data past the bank's end");
    motions_.push_back(std::move(m));
  }
}

const MotionBank::Motion* MotionBank::find(int act, int sub, int track) const {
  const uint32_t key = (uint32_t(act) << 16) | ((sub & 0xFF) << 8) | (track & 0xFF);
  for (const Motion& m : motions_)
    if (m.key == key) return &m;
  return nullptr;
}

std::map<std::string, MotionBank::Sample> MotionBank::sample(const Motion& m, double frame) const {
  std::map<std::string, Sample> out;
  for (const Track& t : m.tracks) {
    const double pos = frame / kRateStep[t.rclass];
    int k0 = int(pos);
    if (k0 >= t.nkeys) k0 = t.nkeys - 1;
    if (k0 < 0) k0 = 0;
    const double f = pos - k0;
    const int k1 = k0 + 1 < t.nkeys ? k0 + 1 : k0;
    auto key = [&](int k, Quat* q, Vec3* tr) {
      *q = unpack_rot(t.rtype, &d_[t.roff + k * rot_size(t.rtype)]);
      if (t.ttype == 1) {
        const uint8_t* p = &d_[t.toff + k * 6];
        *tr = {half(le16(p)), half(le16(p + 2)), half(le16(p + 4))};
      } else if (t.ttype == 2) {
        const uint8_t* p = &d_[t.toff + k * 12];
        *tr = {bef(p), bef(p + 4), bef(p + 8)};
      }
    };
    Sample s{};
    s.has_t = t.ttype == 1 || t.ttype == 2;
    Quat q0;
    Vec3 t0{0, 0, 0};
    key(k0, &q0, &t0);
    if (f <= 1e-4 || k1 == k0) {
      s.q = q0, s.t = t0;
    } else {
      Quat q1;
      Vec3 t1{0, 0, 0};
      key(k1, &q1, &t1);
      s.q = slerp(q0, q1, f);
      s.t = t0 + (t1 - t0) * f;
    }
    out[t.name] = s;
  }
  return out;
}

Frames retarget_motion(const Retarget& rt, const MotionBank& bank, const MotionBank::Motion& m, const std::vector<double>* times,
                       double hip) {
  const Character& J = rt.J.base;
  const Character& B = rt.B;
  std::vector<double> all;
  if (!times) {
    for (int f = 0; f < m.frames; ++f) all.push_back(f);
    times = &all;
  }
  Frames out;
  const size_t nj = J.names.size(), nb = B.names.size();
  std::vector<Mat3> W(nj), G(nb);
  for (double f : *times) {
    const auto sample = bank.sample(m, f);
    for (size_t i = 0; i < nj; ++i) {
      Mat3 lr = J.local_R[i];
      const auto it = sample.find(J.names[i]);
      if (it != sample.end()) lr = quat_mat(it->second.q);
      const int p = J.parent[i];
      W[i] = (p >= 0 && p < int(i)) ? W[p] * lr : lr;
    }
    Pose pose;
    for (size_t i = 0; i < nb; ++i) {
      const int p = B.parent[i], a = rt.anchor[i];
      if (a < 0) {
        G[i] = (p >= 0 ? G[p] : Mat3::identity()) * B.local_R[i];
        continue;
      }
      const Mat3 D = W[a] * transpose(J.world_R[a]);
      G[i] = rt.R[i] * D * transpose(rt.R[i]) * B.world_R[i];
      pose.local[int(i)] = p >= 0 ? transpose(G[p]) * G[i] : G[i];
    }
    const auto r = sample.find(J.names[0]);
    pose.root = (r != sample.end() && r->second.has_t) ? r->second.t * hip : B.local_pos[0];
    out.push_back(std::move(pose));
  }
  return out;
}

Frames mirror_frames(const Character& B, const Frames& in) {
  Mat3 M;
  M.m = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
  const std::vector<int> twin = twin_index(B);
  const size_t n = B.names.size();
  Frames out;
  for (const Pose& p : in) {
    std::vector<Mat3> G(n), D(n), G2(n);
    for (size_t i = 0; i < n; ++i) {
      const auto it = p.local.find(int(i));
      const Mat3& lr = it != p.local.end() ? it->second : B.local_R[i];
      G[i] = B.parent[i] < 0 ? lr : G[B.parent[i]] * lr;
      D[i] = G[i] * transpose(B.world_R[i]);
    }
    Pose q;
    for (size_t j = 0; j < n; ++j) {
      const int par = B.parent[j], i = twin[j];
      if (p.local.count(i) || p.local.count(int(j))) {
        G2[j] = M * D[i] * M * B.world_R[j];
        q.local[int(j)] = par < 0 ? G2[j] : transpose(G2[par]) * G2[j];
      } else {
        G2[j] = (par >= 0 ? G2[par] : Mat3::identity()) * B.local_R[j];
      }
    }
    q.root = {-p.root[0], p.root[1], p.root[2]};
    out.push_back(std::move(q));
  }
  return out;
}

Frames bank_frames(const Character& B, const MotionBank& bank, const MotionBank::Motion& m) {
  Frames out;
  for (int f = 0; f < m.frames; ++f) {
    Pose p;
    p.root = B.local_pos[0];
    for (const auto& [name, s] : bank.sample(m, f)) {
      const int i = B.bone(name);
      if (i < 0) continue;
      p.local[i] = quat_mat(s.q);
      if (i == 0 && s.has_t) p.root = s.t;
    }
    out.push_back(std::move(p));
  }
  return out;
}

std::pair<char, int> hit_frame(const Character& B, const Frames& f) {
  const int chest = B.bone("mune"), lh = B.bone("l_te"), rh = B.bone("r_te");
  if (chest < 0 || lh < 0 || rh < 0 || f.empty()) return {'r', 0};
  std::vector<double> reach[2];
  for (const Pose& p : f) {
    const std::vector<Vec3> T = rs_world(B, p);
    reach[0].push_back(-(T[lh] - T[chest])[2]);
    reach[1].push_back(-(T[rh] - T[chest])[2]);
  }
  int frame[2];
  double gain[2];
  for (int s = 0; s < 2; ++s) {
    frame[s] = int(std::max_element(reach[s].begin(), reach[s].end()) - reach[s].begin());
    gain[s] = reach[s][frame[s]] - reach[s][0];
  }
  return gain[0] > gain[1] ? std::pair<char, int>{'l', frame[0]} : std::pair<char, int>{'r', frame[1]};
}

std::vector<double> warp_times(int n_dst, int dst_peak, int n_src, int src_peak) {
  std::vector<double> out;
  for (int f = 0; f < n_dst; ++f) {
    if (f <= dst_peak) out.push_back(f * double(src_peak) / std::max(dst_peak, 1));
    else out.push_back(src_peak + (f - dst_peak) * double(n_src - 1 - src_peak) / std::max(n_dst - 1 - dst_peak, 1));
  }
  return out;
}

std::vector<double> resample_times(int n_src, int n_dst) {
  std::vector<double> out;
  for (int f = 0; f < n_dst; ++f) out.push_back(f * double(n_src - 1) / std::max(n_dst - 1, 1));
  return out;
}

void BankWriter::add(int act, int sub, Frames frames) {
  ours_.emplace_back((uint32_t(act) << 16) | (uint32_t(sub) << 8), std::move(frames));
}

Bytes BankWriter::build() const {
  constexpr int kOurSet = 7;
  if (ours_.empty()) throw Error("no motions to write");
  const Bytes& rs = base_.data();
  const size_t NS = base_.names_size();
  const uint32_t max_sets = be16(&rs[NS + 2]);
  const size_t desc_base = NS + 4 + size_t(max_sets) * 4;
  std::vector<std::pair<uint16_t, uint16_t>> sets;
  size_t desc_end = 0;
  for (uint32_t i = 0; i < max_sets; ++i) {
    sets.emplace_back(be16(&rs[NS + 4 + i * 4]), be16(&rs[NS + 6 + i * 4]));
    desc_end = std::max(desc_end, size_t(sets.back().first) + sets.back().second);
  }
  if (int(max_sets) <= kOurSet || sets[kOurSet].first != 0) throw Error("the base motion bank has no free track set");
  std::vector<int> bones;
  for (const auto& [i, m] : ours_[0].second[0].local) bones.push_back(i);
  sets[kOurSet] = {uint16_t(bones.size()), uint16_t(desc_end)};
  Bytes head;
  app_be16(head, 0);
  app_be16(head, max_sets);
  for (const auto& [c, s] : sets) {
    app_be16(head, c);
    app_be16(head, s);
  }
  head.insert(head.end(), rs.begin() + desc_base, rs.begin() + desc_base + desc_end * 4);
  const std::vector<std::string>& names = base_.bones();
  for (int i : bones) {
    const auto it = std::find(names.begin(), names.end(), B_.names[i]);
    if (it == names.end()) throw Error("the base motion bank has no bone " + B_.names[i]);
    app_be16(head, uint32_t(it - names.begin()));
    head.push_back(0x10);                    // rate class 0, 5-byte rotations
    head.push_back(i == 0 ? 0x02 : 0x00);    // root: float translation
  }
  const size_t dir_off = (head.size() + 15) & ~size_t(15);
  put_be16(&head[0], uint32_t(dir_off));
  head.resize(dir_off, 0);
  std::vector<const std::pair<uint32_t, Frames>*> order;
  for (const auto& o : ours_) order.push_back(&o);
  std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->first < b->first; });
  Bytes dir, data;
  for (const auto* o : order) {
    const Frames& fr = o->second;
    app_be32(dir, o->first);
    app_be32(dir, uint32_t(data.size()));
    app_be16(dir, uint32_t(fr.size()));
    dir.push_back(kOurSet);
    dir.push_back(0);
    app_be32(dir, 0xFFFFFFFFu);
    for (int k = 0; k < 3; ++k) app_be32(dir, 0);
    app_be16(dir, 0);
    app_be16(dir, 0);
    for (int i : bones) {
      if (i == 0)
        for (const Pose& p : fr)
          for (int k = 0; k < 3; ++k) app_bef(data, p.root[k]);
      for (const Pose& p : fr) {
        const auto it = p.local.find(i);
        pack_rot(mat_quat(it != p.local.end() ? it->second : B_.local_R[i]), data);
      }
    }
  }
  Bytes out(rs.begin(), rs.begin() + NS);
  app(out, head);
  app_be32(out, uint32_t(order.size()));
  app(out, dir);
  app(out, data);
  return out;
}

}  // namespace rs::primport
