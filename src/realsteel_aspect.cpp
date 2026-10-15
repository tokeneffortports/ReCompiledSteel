// realsteel - the game's culling for picture shapes other than 16:9
// (2026-10-12).
//
// The renderer shows more than the game's 16:9 view (taller at 4:3, wider at
// 21:9, see vk_backend.cpp), but the game still culled to its own 16:9
// frustum: things in the extra strips popped in and out (seen on a
// 4:3 screen). sub_82104688 builds the view frustum for culling (this r3, fov f1 -
// vertical, its half used as an angle; near f2, far f3; horizontal half =
// fov / 2 * 16/9, picked from the video mode as the projection does in
// sub_821B03E8). Its fov is widened so the frustum covers the whole picture;
// the projection the picture is drawn with is untouched.

#include <algorithm>
#include <cmath>

#include <rex/cvar.h>

#include "realsteel_native_present.h"

#include "generated/default_final/realsteel_init.h"

REXCVAR_DEFINE_BOOL(realsteel_cull_whole_picture, true, "Real Steel",
                    "Picture shapes other than 16:9: cull to the whole picture, not the "
                    "game's 16:9 view (off shows the pop-in in the extra strips)");

extern "C" REX_FUNC(sub_82104688) {
  const float aspect = realsteel::native_present::CurrentPictureAspect();
  if (aspect > 0.0f && REXCVAR_GET(realsteel_cull_whole_picture)) {
    const double game = 16.0 / 9.0;
    const double half = ctx.f1.f64 * 0.5;
    double wanted = half;
    if (aspect < game) {
      // Taller: the vertical extent grows by game / aspect.
      wanted = std::atan(std::tan(half) * game / aspect);
    } else {
      // Wider: the horizontal one grows by aspect / game.
      wanted = std::atan(std::tan(std::min(half * game, 1.5)) * aspect / game) / game;
    }
    wanted = std::min(wanted, 1.45 / game);  // keep both angles well below 90 degrees
    ctx.f1.f64 = std::max(half, wanted) * 2.0;
  }
  __imp__sub_82104688(ctx, base);
}
