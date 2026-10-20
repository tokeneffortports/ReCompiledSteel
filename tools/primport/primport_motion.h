// Fighter import: Yuke's motion banks (Real Steel / Pacific Rim), reading,
// conversion of a fighter's motions onto its converted body (the Real Steel
// skeleton) and writing a bank of them.
//
// Bank layout: 0x1000 (Real Steel) / 0x4000 (Pacific Rim) bytes of 16-byte
// bone names; at NS: u16 directory offset, u16 set count, sets (u16 count,
// u16 first descriptor), descriptors (u16 bone, u8 rate class | rotation
// type << 4, u8 translation type | scale type << 4); at NS + directory
// offset: u32 count, 32-byte entries (u32 key act << 16 | sub << 8 | track,
// u32 data offset from the end of the directory, u16 frames, u8 set, u8
// flag, ...). 30 Hz keys: translation, rotation (5-byte axis-angle), scale.
#pragma once

#include "primport_model.h"

namespace rs::primport {

class MotionBank {
 public:
  explicit MotionBank(Bytes data);
  struct Track {
    int bone;
    std::string name;
    int rclass, rtype, ttype, stype;
    int nkeys;
    size_t toff, roff;
  };
  struct Motion {
    uint32_t key;
    int act, sub, track;
    int frames;
    int set;
    std::vector<Track> tracks;
  };
  const std::vector<Motion>& motions() const { return motions_; }
  const Motion* find(int act, int sub = 0, int track = 0) const;
  // bone name -> (rotation, translation if the track has one)
  struct Sample {
    Quat q;
    bool has_t;
    Vec3 t;
  };
  std::map<std::string, Sample> sample(const Motion& m, double frame) const;
  const Bytes& data() const { return d_; }
  const std::vector<std::string>& bones() const { return bones_; }
  size_t names_size() const { return ns_; }

 private:
  Bytes d_;
  size_t ns_ = 0x1000;
  std::vector<std::string> bones_;
  std::vector<Motion> motions_;
};

// one converted pose: root translation + local rotations of the Real Steel
// bones that carry fighter geometry
struct Pose {
  Vec3 root;
  std::map<int, Mat3> local;
};
using Frames = std::vector<Pose>;

// The fighter's motion on its converted body at the given 30 Hz times
// (default: every frame). hip: Real Steel standing hip height / the fighter's.
Frames retarget_motion(const Retarget& rt, const MotionBank& bank, const MotionBank::Motion& m, const std::vector<double>* times,
                       double hip);
Frames mirror_frames(const Character& B, const Frames& f);
// A Real Steel bank motion as poses of the Real Steel skeleton.
Frames bank_frames(const Character& B, const MotionBank& bank, const MotionBank::Motion& m);
// The striking hand ('l' / 'r') and the frame it reaches furthest forward.
std::pair<char, int> hit_frame(const Character& B, const Frames& f);
std::vector<double> warp_times(int n_dst, int dst_peak, int n_src, int src_peak);
std::vector<double> resample_times(int n_src, int n_dst);

// A bank with the Real Steel base bank's header (bone names, its track sets
// unchanged) plus one set of the converted bones; motions keyed act/sub/0.
class BankWriter {
 public:
  BankWriter(const MotionBank& base, const Character& B) : base_(base), B_(B) {}
  void add(int act, int sub, Frames frames);
  Bytes build() const;

 private:
  const MotionBank& base_;
  const Character& B_;
  std::vector<std::pair<uint32_t, Frames>> ours_;
};

}  // namespace rs::primport
