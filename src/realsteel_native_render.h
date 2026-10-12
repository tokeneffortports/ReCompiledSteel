// Real Steel native-render migration layer.
//
// Phase 1 records the title's D3D draw submissions at stable guest function
// boundaries while leaving the Xenos path fully operational. Later phases
// consume the same records from the native guest-output callback.

#pragma once

#include <cstdint>

namespace realsteel::native_render {

void Install();

// Called at the title's 67-way render-command dispatcher. The command is
// copied while its guest backing storage is stable, before Xenos translation.
void OnRenderCommand(uint8_t* base, uint32_t command);

// Called after guest D3D SetViewport (core, 0x82253770) has updated the
// device's viewport shadow; injects a synthetic type-200 command record.
void OnViewportSet(uint8_t* base, uint32_t device);

// Called on guest D3D SetVertexShaderConstantF / SetPixelShaderConstantF
// before the original runs; injects synthetic type-201/202 records so
// constants set outside the 67-type stream reach the replay.
void OnShaderConstF(uint8_t* base, bool is_pixel, uint32_t start_reg,
                    uint32_t data_va, uint32_t vec4_count);

// Called on guest D3D SetStreamSource (0x82253260), before the original runs.
// Maintains a host-side per-thread stream shadow for indexed-draw capture.
void OnSetStreamSource(uint32_t device, uint32_t stream, uint32_t vb,
                       uint32_t offset_bytes, uint32_t stride_bytes);

// Called after the guest's D3D draw routine has flushed its pending state.
void OnDrawIndexed(uint8_t* base, uint32_t device, uint32_t primitive_type,
                   uint32_t arg5, uint32_t arg6, uint32_t index_count,
                   uint32_t caller);

// Called after BeginVertices has reserved the inline vertex payload and
// returned its guest write pointer.
void OnBeginVertices(uint8_t* base, uint32_t device, uint32_t primitive_type,
                     uint32_t vertex_count, uint32_t vertex_stride,
                     uint32_t guest_data, uint32_t caller);

// Called immediately before the original D3D swap routine.
void OnFrameEnd(uint8_t* base);

// D3D Resolve (sub_8224E748, post-call): records the destination texture as
// a resolve of the current command. Replaces the dispatcher-command (type
// 53) capture, and also sees the resolves issued from inside other commands
// (sub-renderer end-flush, user callbacks).
void OnD3DResolve(uint8_t* base, uint32_t dest_texture);

}  // namespace realsteel::native_render
