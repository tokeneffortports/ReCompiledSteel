// Real Steel shader capture — cross-module accessors.

#pragma once

#include <cstdint>
#include <vector>

namespace realsteel::shader_dump {

// Guest VAs (0xB0xxxxxx physical mirror) of the most recently IM_LOADed
// vertex/pixel shader microcode, i.e. what the GPU is actually running.
// 0 = nothing uploaded yet. Filenames in rs_shaders/ embed these VAs, so
// capture records correlate directly with dumped shaders. Updated by the
// sub_8225E108 flush hook; requires the realsteel_shader_dump cvar (default
// on) — with it off these go stale.
uint32_t LastVertexShaderVa();
uint32_t LastPixelShaderVa();
// Microcode VA of the shader object currently bound via
// SetVertexShader/SetPixelShader (falls back to Last*ShaderVa when the
// object has not been seen in an IM_LOAD yet). Prefer these per draw.
uint32_t BoundVertexShaderVa();
uint32_t BoundPixelShaderVa();

// Live path (2026-09-06): the (microcode VA -> container hash) pairing the
// dumper appends to rs_shaders/containers/manifest.txt, available in memory
// as soon as the shader is loaded — the in-process renderer resolves this
// session's shader addresses through it instead of a manifest read at
// start-up (which cost the fight all its 3D draws).
bool LookupUcode(uint32_t ucode_va, uint64_t* container_hash, bool* is_vs);
// The bytes of a container seen this session (as the game loaded it, the
// vfetch block patched for the current vertex declaration). The renderer
// hashes these for its cache lookup instead of reading rs_shaders/ files.
bool ContainerBytes(uint64_t container_hash, std::vector<uint8_t>* out);

// Container hash of the program last loaded (IM_LOAD or IM_LOAD_IMMEDIATE)
// for the currently bound vertex / pixel shader object; 0 when unknown.
uint64_t BoundShaderContainer(bool vertex);

}  // namespace realsteel::shader_dump
