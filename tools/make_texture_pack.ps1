# Makes a texture pack from the game's exported textures with an AI upscaler
# (Real-ESRGAN ncnn-vulkan, which you download yourself:
# https://github.com/xinntao/Real-ESRGAN/releases).
#
# 1. In the game: Settings > Enhancements > Export textures on, play the
#    screens you want covered, switch it off.
# 2. From the game's folder:
#      powershell -ExecutionPolicy Bypass -File make_texture_pack.ps1 -Upscaler C:\path\realesrgan-ncnn-vulkan.exe
#    (-Scale 2 is the default and suits phones / handhelds; -Scale 4 for a
#    strong PC GPU. Run it again after exporting more: textures already in
#    the pack are kept.)
# 3. In the game: Settings > Enhancements > Texture pack on.
#
# Normal maps (bluish or yellow-green lighting data), tiny and flat textures
# are left out: the game keeps using the originals for those.
param(
  [Parameter(Mandatory = $true)][string]$Upscaler,
  [string]$Export = "texture_export",
  [string]$Pack = "texture_pack",
  [ValidateSet(2, 4)][int]$Scale = 2,
  [int]$MaxSize = 4096,               # largest side of a replacement
  [string]$Model = "realesrgan-x4plus",
  [switch]$Force                      # redo textures already in the pack
)
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class RsTexPack {
  // BGRA bytes, not premultiplied.
  static byte[] Load(string path, out int w, out int h) {
    using (var src = new Bitmap(path)) {
      w = src.Width; h = src.Height;
      var rect = new Rectangle(0, 0, w, h);
      using (var bmp = src.Clone(rect, PixelFormat.Format32bppArgb)) {
        var d = bmp.LockBits(rect, ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var buf = new byte[w * h * 4];
        for (int y = 0; y < h; ++y) Marshal.Copy(d.Scan0 + y * d.Stride, buf, y * w * 4, w * 4);
        bmp.UnlockBits(d);
        return buf;
      }
    }
  }

  static void Save(string path, byte[] buf, int w, int h) {
    using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb)) {
      var rect = new Rectangle(0, 0, w, h);
      var d = bmp.LockBits(rect, ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
      for (int y = 0; y < h; ++y) Marshal.Copy(buf, y * w * 4, d.Scan0 + y * d.Stride, w * 4);
      bmp.UnlockBits(d);
      bmp.Save(path, ImageFormat.Png);
    }
  }

  // "" = worth upscaling, otherwise why not.
  public static string Classify(string path) {
    int w, h;
    var p = Load(path, out w, out h);
    if (Math.Min(w, h) < 16 || w * h < 1024) return "tiny";
    double[] sum = new double[4], sq = new double[4];
    int n = w * h;
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < 4; ++c) { double v = p[i * 4 + c]; sum[c] += v; sq[c] += v * v; }
    double[] mean = new double[4], sd = new double[4];
    for (int c = 0; c < 4; ++c) {
      mean[c] = sum[c] / n;
      sd[c] = Math.Sqrt(Math.Max(0.0, sq[c] / n - mean[c] * mean[c]));
    }
    if (sd[0] < 3 && sd[1] < 3 && sd[2] < 3 && sd[3] < 3) return "flat";
    // Normal maps hold directions: xyz in rgb (bluish, length 1) or xy only
    // with no blue (yellow-green, length up to 1). Nearly every texel fits.
    int xyz = 0, xy = 0;
    for (int i = 0; i < n; ++i) {
      double x = p[i * 4 + 2] / 127.5 - 1.0, y = p[i * 4 + 1] / 127.5 - 1.0, z = p[i * 4] / 127.5 - 1.0;
      double l2 = x * x + y * y + z * z;
      if (z > 0.0 && l2 > 0.6 && l2 < 1.4) ++xyz;
      if (p[i * 4] < 64 && x * x + y * y < 1.1) ++xy;
    }
    if (xyz > n * 0.9 || xy > n * 0.9) return "normal map";
    return "";
  }

  // Shrinks by a whole factor (box filter weighted by alpha, so fully
  // transparent texels do not darken the edges of what is visible).
  public static void Shrink(string src, string dst, int f) {
    int w, h;
    var p = Load(src, out w, out h);
    int ow = w / f, oh = h / f;
    var o = new byte[ow * oh * 4];
    for (int y = 0; y < oh; ++y) {
      for (int x = 0; x < ow; ++x) {
        double sa = 0, sb = 0, sg = 0, sr = 0, pb = 0, pg = 0, pr = 0;
        for (int j = 0; j < f; ++j) {
          int row = ((y * f + j) * w + x * f) * 4;
          for (int i = 0; i < f; ++i) {
            int k = row + i * 4;
            double a = p[k + 3];
            sa += a; sb += p[k] * a; sg += p[k + 1] * a; sr += p[k + 2] * a;
            pb += p[k]; pg += p[k + 1]; pr += p[k + 2];
          }
        }
        int ff = f * f, q = (y * ow + x) * 4;
        if (sa > 0) {
          o[q] = (byte)Math.Round(sb / sa); o[q + 1] = (byte)Math.Round(sg / sa); o[q + 2] = (byte)Math.Round(sr / sa);
        } else {
          o[q] = (byte)Math.Round(pb / ff); o[q + 1] = (byte)Math.Round(pg / ff); o[q + 2] = (byte)Math.Round(pr / ff);
        }
        o[q + 3] = (byte)Math.Round(sa / ff);
      }
    }
    Save(dst, o, ow, oh);
  }
}
'@

if (-not (Test-Path $Upscaler)) { throw "Upscaler not found: $Upscaler" }
if (-not (Test-Path $Export)) { throw "No $Export folder - switch on Export textures in the game first." }
$Export = (Resolve-Path $Export).Path
New-Item -ItemType Directory -Force $Pack | Out-Null
$Pack = (Resolve-Path $Pack).Path
$work = Join-Path ([System.IO.Path]::GetTempPath()) ("rs_texture_pack_" + [guid]::NewGuid().ToString("N"))
$in = Join-Path $work "in"; $out = Join-Path $work "out"
New-Item -ItemType Directory -Force $in, $out | Out-Null

$skipped = @{}
$queued = 0; $kept = 0
foreach ($f in Get-ChildItem $Export -Filter *.png) {
  if (-not $Force -and (Test-Path (Join-Path $Pack $f.Name))) { $kept++; continue }
  $why = [RsTexPack]::Classify($f.FullName)
  if ($why -ne "") { $skipped[$why] = 1 + [int]$skipped[$why]; continue }
  Copy-Item $f.FullName $in
  $queued++
}
"{0} to upscale, {1} already in the pack" -f $queued, $kept
foreach ($k in $skipped.Keys) { "  left out ({0}): {1}" -f $k, $skipped[$k] }

if ($queued -gt 0) {
  "Upscaling with $Model (4x) ..."
  $log = Join-Path $work "upscaler.log"
  $p = Start-Process -FilePath $Upscaler -ArgumentList @("-i", "`"$in`"", "-o", "`"$out`"", "-n", $Model, "-s", "4", "-f", "png") `
         -NoNewWindow -Wait -PassThru -RedirectStandardError $log -RedirectStandardOutput "$log.out"
  if ($p.ExitCode -ne 0) { Get-Content $log -Tail 20; throw "The upscaler failed (exit code $($p.ExitCode))" }
  $made = 0
  foreach ($f in Get-ChildItem $out -Filter *.png) {
    # name: <hash>_<W>x<H> (the original's size)
    if ($f.BaseName -notmatch '_(\d+)x(\d+)$') { continue }
    $w = [int]$Matches[1]; $h = [int]$Matches[2]
    $shrink = 4 / $Scale
    while (($w * 4 / $shrink) -gt $MaxSize -or ($h * 4 / $shrink) -gt $MaxSize) { $shrink *= 2 }
    if ($shrink -ge 4) { continue }  # no bigger than the original: not worth it
    $dst = Join-Path $Pack $f.Name
    if ($shrink -eq 1) { Copy-Item $f.FullName $dst -Force } else { [RsTexPack]::Shrink($f.FullName, $dst, $shrink) }
    $made++
  }
  "$made replacement texture(s) written to $Pack"
}
Remove-Item -Recurse -Force $work
"Done. In the game: Settings > Enhancements > Texture pack on."
