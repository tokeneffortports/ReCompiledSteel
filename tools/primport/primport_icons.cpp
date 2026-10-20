// Fighter import: select-screen icons (see primport_icons.h).
#include "primport_icons.h"

#include <algorithm>
#include <cstring>

namespace rs::primport {

namespace {
const char* const kSizes[] = {"0000", "0001", "0002"};
const double kBackground[3][3] = {{0, 62, 112}, {110, 6, 6}, {40, 150, 175}};   // measured on the stock icons
constexpr double kGipsyHeight = 88.1;   // the icon framing below was tuned on a model this tall

Vec3 normalized(Vec3 v) { return v * (1.0 / norm(v)); }
double frac(double v) { return v - std::floor(v); }
}  // namespace

Image render(const std::vector<RenderMesh>& meshes, const Image& texture, const Image* glow, Vec3 centre, double half_w,
             double half_h, int width, int height, double yaw, double pitch, int ss, double gain) {
  const int W = width * ss, H = height * ss;
  const Mat3 R = rot_x(pitch) * rot_y(yaw);
  std::vector<float> colour(size_t(W) * H * 3, 0.0f), alpha(size_t(W) * H, 0.0f), zbuf(size_t(W) * H, 1e30f);
  const Vec3 light = normalized({-0.55, -0.6, -0.6}), fill = normalized({0.7, -0.2, -0.4});
  for (const RenderMesh& m : meshes) {
    const size_t nv = m.pos.size();
    std::vector<double> sx(nv), sy(nv), sz(nv);
    std::vector<Vec3> n(nv);
    for (size_t i = 0; i < nv; ++i) {
      const Vec3 p = R * (m.pos[i] - centre);
      n[i] = R * m.nrm[i];
      sx[i] = (p[0] / half_w * 0.5 + 0.5) * W;
      sy[i] = (p[1] / half_h * 0.5 + 0.5) * H;
      sz[i] = p[2];
    }
    for (const auto& t : m.tris) {
      const int a = t[0], b = t[1], c = t[2];
      const double xs[3] = {sx[a], sx[b], sx[c]}, ys[3] = {sy[a], sy[b], sy[c]};
      const int x0 = std::max(int(std::min({xs[0], xs[1], xs[2]})), 0), x1 = std::min(int(std::max({xs[0], xs[1], xs[2]})) + 1, W - 1);
      const int y0 = std::max(int(std::min({ys[0], ys[1], ys[2]})), 0), y1 = std::min(int(std::max({ys[0], ys[1], ys[2]})) + 1, H - 1);
      if (x0 > x1 || y0 > y1) continue;
      const double d = (xs[1] - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (ys[1] - ys[0]);
      if (std::abs(d) < 1e-9) continue;
      for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
          const double X = x + 0.5, Y = y + 0.5;
          const double w1 = ((X - xs[0]) * (ys[2] - ys[0]) - (xs[2] - xs[0]) * (Y - ys[0])) / d;
          const double w2 = ((xs[1] - xs[0]) * (Y - ys[0]) - (X - xs[0]) * (ys[1] - ys[0])) / d;
          const double w0 = 1 - w1 - w2;
          if (w0 < 0 || w1 < 0 || w2 < 0) continue;
          const double z = w0 * sz[a] + w1 * sz[b] + w2 * sz[c];
          const size_t pi = size_t(y) * W + x;
          if (!(z < zbuf[pi])) continue;
          zbuf[pi] = float(z);
          const double u = w0 * m.uv[a][0] + w1 * m.uv[b][0] + w2 * m.uv[c][0];
          const double v = w0 * m.uv[a][1] + w1 * m.uv[b][1] + w2 * m.uv[c][1];
          const int tx = std::clamp(int(frac(u) * texture.w), 0, texture.w - 1);
          const int ty = std::clamp(int(frac(v) * texture.h), 0, texture.h - 1);
          Vec3 nn = n[a] * w0 + n[b] * w1 + n[c] * w2;
          nn = nn * (1.0 / std::max(norm(nn), 1e-9));
          if (nn[2] > 0) nn = nn * -1.0;      // face the camera
          const double lam = std::clamp(-dot(nn, light), 0.0, 1.0), fl = std::clamp(-dot(nn, fill), 0.0, 1.0);
          const double rim = std::pow(std::clamp(1 - std::abs(nn[2]), 0.0, 1.0), 3);
          const uint8_t* tp = texture.at(tx, ty);
          const double shade = 0.38 + 1.15 * lam + 0.35 * fl;
          double col[3];
          const double rimc[3] = {0.6, 0.85, 1.0}, glowc[3] = {0.45, 0.9, 1.0};
          double gv = 0;
          if (glow) {
            const int gx = std::clamp(int(frac(u) * glow->w), 0, glow->w - 1), gy = std::clamp(int(frac(v) * glow->h), 0, glow->h - 1);
            const uint8_t* g = glow->at(gx, gy);
            gv = std::max({g[0], g[1], g[2]}) / 255.0;
          }
          for (int k = 0; k < 3; ++k) {
            col[k] = tp[k] / 255.0 * shade * gain + 0.25 * rim * rimc[k];
            if (glow) col[k] += gv * glowc[k] * 1.6;     // the glow mask is zones; tinted cyan
            colour[pi * 3 + k] = float(std::clamp(col[k], 0.0, 1.0));
          }
          alpha[pi] = 1.0f;
        }
      }
    }
  }
  Image out;
  out.w = width, out.h = height;
  out.px.assign(size_t(width) * height * 4, 0);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      double c[3] = {0, 0, 0}, a = 0;
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          const size_t pi = size_t(y * ss + dy) * W + (x * ss + dx);
          for (int k = 0; k < 3; ++k) c[k] += colour[pi * 3 + k];
          a += alpha[pi];
        }
      const double cnt = double(ss * ss);
      a /= cnt;
      uint8_t* o = out.at(x, y);
      for (int k = 0; k < 3; ++k) o[k] = uint8_t(std::clamp(c[k] / cnt / std::max(a, 1e-6) * 255, 0.0, 255.0));
      o[3] = uint8_t(std::clamp(a * 255, 0.0, 255.0));
    }
  return out;
}

namespace {

struct Frame {
  Image frame;
  std::vector<uint8_t> window;    // 1 = the portrait shows here
  int x0, y0, x1, y1;             // the window's box (exclusive)
};

Frame frame_parts(const Archive& arc, const std::string& size) {
  std::vector<std::pair<std::string, Bytes>> kids;
  if (!arc.children("/MENU/SSFD/" + size, &kids)) throw Error("the game's DLC_HD.pac has no select icons");
  std::vector<Image> imgs;
  for (const auto& [name, d] : kids) {
    if (name >= "0000" && name <= "0006") continue;   // garage icons carry their own text
    imgs.push_back(dds_decode(d));
  }
  if (imgs.size() < 2) throw Error("too few stock select icons");
  Frame f;
  const int w = imgs[0].w, h = imgs[0].h;
  f.frame.w = w, f.frame.h = h;
  f.frame.px.assign(size_t(w) * h * 4, 0);
  f.window.assign(size_t(w) * h, 0);
  std::vector<int> vals(imgs.size());
  f.x0 = w, f.y0 = h, f.x1 = 0, f.y1 = 0;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      int spread = 0;
      for (int c = 0; c < 4; ++c) {
        int lo = 255, hi = 0;
        for (size_t i = 0; i < imgs.size(); ++i) {
          vals[i] = imgs[i].at(x, y)[c];
          lo = std::min(lo, vals[i]), hi = std::max(hi, vals[i]);
        }
        if (c < 3) spread = std::max(spread, hi - lo);
        std::sort(vals.begin(), vals.end());
        const size_t n = vals.size();
        const double med = n % 2 ? vals[n / 2] : (vals[n / 2 - 1] + vals[n / 2]) / 2.0;
        f.frame.at(x, y)[c] = uint8_t(med);
      }
      if (spread >= 24 && f.frame.at(x, y)[3] > 0) {
        f.window[size_t(y) * w + x] = 1;
        f.x0 = std::min(f.x0, x), f.y0 = std::min(f.y0, y), f.x1 = std::max(f.x1, x + 1), f.y1 = std::max(f.y1, y + 1);
      }
    }
  if (f.x1 <= f.x0) throw Error("no portrait window in the stock icons");
  return f;
}

Image compose(const Frame& f, const double bg[3], const Image& portrait) {
  Image out = f.frame;
  const int bw = f.x1 - f.x0, bh = f.y1 - f.y0;
  for (int y = 0; y < bh; ++y)
    for (int x = 0; x < bw; ++x) {
      if (!f.window[size_t(f.y0 + y) * f.frame.w + f.x0 + x]) continue;
      const double grad = 1.05 - 0.45 * (double(y) / std::max(bh - 1, 1));
      const double stripes = 1.0 - 0.12 * (((x + y) / 2) % 2);
      const uint8_t* p = portrait.at(x, y);
      const double a = p[3] / 255.0;
      uint8_t* o = out.at(f.x0 + x, f.y0 + y);
      for (int k = 0; k < 3; ++k) {
        const double b = std::clamp(bg[k] * grad * stripes, 0.0, 255.0);
        o[k] = uint8_t(std::clamp(p[k] * a + b * (1 - a), 0.0, 255.0));
      }
      o[3] = 255;
    }
  return out;
}

}  // namespace

std::map<std::string, Bytes> make_icons(const Archive& stock, const Posed& fighter) {
  const std::vector<MeshData> md = fighter.meshes(true);
  std::vector<RenderMesh> meshes;
  std::vector<double> xs;
  double ymin = 1e30, ymax = -1e30;
  for (const MeshData& m : md) {
    meshes.push_back({m.pos, m.nrm, m.uv, m.tris});
    for (const Vec3& p : m.pos) xs.push_back(p[0]), ymin = std::min(ymin, p[1]), ymax = std::max(ymax, p[1]);
  }
  const auto& tex = fighter.base.textures;
  if (!tex.count("color")) throw Error("fighter has no colour texture");
  const Image texture = dds_decode(tex.at("color"));
  Image glow;
  const bool has_glow = tex.count("glowmask") > 0;
  if (has_glow) glow = dds_decode(tex.at("glowmask"));
  const double k = (ymax - ymin) / kGipsyHeight;
  const int head = fighter.base.bone("Head");
  double top = ymin - 1.0 * k;                       // y points down
  if (head >= 0) top = std::min(fighter.world_T[head][1] - 11.0 * k, top);
  std::sort(xs.begin(), xs.end());
  const size_t n = xs.size();
  const double cx = n % 2 ? xs[n / 2] : (xs[n / 2 - 1] + xs[n / 2]) / 2;
  std::map<std::string, Bytes> out;
  std::map<std::pair<int, int>, Image> cache;
  for (int s = 0; s < 3; ++s) {
    const Frame f = frame_parts(stock, kSizes[s]);
    const int w = f.x1 - f.x0, h = f.y1 - f.y0;
    auto it = cache.find({w, h});
    if (it == cache.end()) {
      const double half_w = 15.0 * k, half_h = half_w * h / w;
      it = cache.emplace(std::pair<int, int>{w, h},
                         render(meshes, texture, has_glow ? &glow : nullptr, {cx, top + half_h, 0.0}, half_w, half_h, w, h, 0.35, -0.12, 3, 2.6))
               .first;
    }
    Bytes tmpl;
    if (!stock.find(std::string("/MENU/SSFD/") + kSizes[s] + "/0064", &tmpl)) throw Error("the game's DLC_HD.pac has no Atom icon");
    out[kSizes[s]] = encode_dxt5(compose(f, kBackground[s], it->second), tmpl);
  }
  return out;
}

}  // namespace rs::primport
