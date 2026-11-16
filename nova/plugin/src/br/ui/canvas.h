// Nova Modular - GPL-3.0-or-later.
//
// The drawing surface the BrickRack module files expect ("ui/canvas.h").
// BrickRack rasterises everything itself into a pixel buffer; here the same
// calls go to Rack's NanoVG context instead, so the modules' custom drawing
// (model names, small read-outs) compiles unchanged. Only the calls the
// panels use are provided.

#pragma once

#include <cstdint>
#include <string>

struct NVGcontext;

namespace br {

inline uint32_t argb(int a, int r, int g, int b) {
  return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}
inline uint32_t withAlpha(uint32_t c, float a) {
  int al = static_cast<int>(((c >> 24) & 0xFF) * (a < 0 ? 0 : (a > 1 ? 1 : a)) + 0.5f);
  return (c & 0x00FFFFFF) | (static_cast<uint32_t>(al) << 24);
}
uint32_t mixColor(uint32_t a, uint32_t b, float t);
uint32_t scaleColor(uint32_t c, float k);  // brightness multiply (keeps alpha)

class Canvas {
 public:
  /// `font` is a NanoVG font handle, or -1 for none (text is then skipped).
  Canvas(NVGcontext* vg, int font) : vg_(vg), font_(font) {}

  void fillRect(float x, float y, float w, float h, uint32_t color);
  void fillRoundRect(float x, float y, float w, float h, float radius, uint32_t color);
  void strokeRoundRect(float x, float y, float w, float h, float radius, float width,
                       uint32_t color);
  void fillCircle(float cx, float cy, float r, uint32_t color);
  void strokeCircle(float cx, float cy, float r, float width, uint32_t color);
  // Radial gradient disc: centre colour -> edge colour. ox/oy move the bright
  // spot, in radii.
  void fillCircleGradient(float cx, float cy, float r, uint32_t inner, uint32_t outer,
                          float ox = 0.f, float oy = 0.f);
  void line(float x0, float y0, float x1, float y1, float width, uint32_t color);
  void polyline(const float* xy, int n, float width, uint32_t color);

  // Text. `size` is the height of the font's em box. align: 0 left, 1 centre,
  // 2 right (br::Align). Returns the advance.
  float text(float x, float y, const std::string& s, float size, uint32_t color,
             int align = 0, bool middle = true);
  float textWidth(const std::string& s, float size);

 private:
  NVGcontext* vg_;
  int font_;
};

}  // namespace br
