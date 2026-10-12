// realsteel - guest GPU services without the xenos plugin (2026-10-02).
//
// With the native renderer drawing every frame from the D3D-level capture,
// the emulated GPU is only needed for what the game's D3D runtime WAITS on:
// the ring buffer's read pointer advancing, fence/scratch values written back
// to memory, command-stream interrupts, occlusion-query results and the
// vertical-blank interrupt. This IGraphicsSystem provides exactly those and
// renders nothing: it walks the PM4 stream, applies the packets with
// guest-visible side effects and skips the rest.
//
// Enabled with --realsteel_native_gpu=true (then --gpu_plugin is ignored);
// see RealsteelApp::OnPreSetup.

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace realsteel::gpu {

bool Enabled();
std::unique_ptr<rex::system::IGraphicsSystem> Create();
// Development: logs every thread's host stack `count` times, a few ms apart,
// from a background thread (Windows only; tools/test/stack_profile.py
// aggregates the "[rs-gpu] stack tid" lines).
void SampleStacks(int count);

}  // namespace realsteel::gpu
