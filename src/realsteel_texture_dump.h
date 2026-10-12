// Real Steel texture capture — reads texture fetch constants from the guest
// D3D device's shadow at draw time, dumps unique textures to rs_textures/,
// and reports per-draw bound texture ids for the frame capture (v3).

#pragma once

#include <cstdint>
#include <vector>

namespace realsteel::texture_dump {

// Sampler slots 0..19: 16 pixel-shader samplers plus the four vertex-texture
// samplers (s16-s19). Capture v10 (2026-09-02); v3-v9 recorded 0..7.
constexpr uint32_t kSlotCount = 20;

// For sampler slots 0..19: reads the 6-dword fetch constant shadow at
// device+1152+slot*24. out_ids[slot] = texture base physical address (the id
// used in rs_textures/ filenames), or 0 if the slot holds no valid
// texture. 2D and cube textures are dumped; cube dumps hold 6 faces at a
// 4 KB-aligned face stride. Dumps each unique texture (fetch-constant + address) once.
// out_fmt[slot] (v11): fetch format (bits 0-5), dimension (bits 8-9),
// endian (bits 12-13) of the bound texture; 0 when the slot is empty. Lets
// the replayer tell depth textures (k_24_8 = 22, k_24_8_FLOAT = 23,
// k_32_FLOAT = 36) from colour ones.
// out_hash (v13, may be null): when |armed| is set, each bound texture's
// LIVE content is hashed (once per texture per frame, keyed by |frame|) and
// dumped if that content was never written before — textures stream in
// after their first bind, so the first-bind dump can be a placeholder. The
// replayer loads tex_<phys>_<hash>.xtx for the frame.
void CaptureBoundTextures(uint8_t* base, uint32_t device,
                          uint32_t out_ids[kSlotCount],
                          uint32_t out_fmt[kSlotCount],
                          uint64_t out_hash[kSlotCount] = nullptr,
                          bool armed = false, uint64_t frame = 0);

// Live path (2026-09-06): the fetch constant last seen bound for |phys|
// (CaptureBoundTextures records one per texture id), and the raw guest
// bytes of that texture (same sizing as the dumper). Both false when the
// id was never bound or the format is unsupported.
// Live path (2026-09-15): while only the in-process renderer consumes the
// frame, CaptureBoundTextures stamps textures cheaply instead of running the
// full dumper per frame (which FNV-hashes every bound texture once per
// frame: 40 ms of a fight frame). Set true while an F10 disk burst is armed.
void SetDiskCaptureArmed(bool armed);

bool LookupFetch(uint32_t phys, uint32_t out_fetch[6]);
bool ReadTextureBytes(uint8_t* base, uint32_t phys, uint32_t out_fetch[6],
                      std::vector<uint8_t>& out);

}  // namespace realsteel::texture_dump
