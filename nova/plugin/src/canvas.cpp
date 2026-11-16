// Nova Modular - GPL-3.0-or-later.
//
// br::Canvas on NanoVG (see br/ui/canvas.h).

#include <algorithm>
#include <cmath>

#include <nanovg.h>

#include "ui/canvas.h"

namespace br {

namespace {

NVGcolor col(uint32_t c) {
  return nvgRGBA((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
}

int clamp255(float v) { return static_cast<int>(std::min(255.f, std::max(0.f, v)) + 0.5f); }

// BrickRack sizes text by the em box; fontstash sizes it by ascent - descent.
// For Barlow Condensed that is 1.2 em.
const float kEmToFontSize = 1.2f;

}  // namespace

uint32_t mixColor(uint32_t a, uint32_t b, float t) {
  t = std::min(1.f, std::max(0.f, t));
  auto ch = [&](int sh) {
    float x = static_cast<float>((a >> sh) & 0xFF), y = static_cast<float>((b >> sh) & 0xFF);
    return static_cast<uint32_t>(clamp255(x + (y - x) * t)) << sh;
  };
  return ch(24) | ch(16) | ch(8) | ch(0);
}

uint32_t scaleColor(uint32_t c, float k) {
  auto ch = [&](int sh) {
    return static_cast<uint32_t>(clamp255(static_cast<float>((c >> sh) & 0xFF) * k)) << sh;
  };
  return (c & 0xFF000000) | ch(16) | ch(8) | ch(0);
}

void Canvas::fillRect(float x, float y, float w, float h, uint32_t color) {
  nvgBeginPath(vg_);
  nvgRect(vg_, x, y, w, h);
  nvgFillColor(vg_, col(color));
  nvgFill(vg_);
}

void Canvas::fillRoundRect(float x, float y, float w, float h, float radius, uint32_t color) {
  nvgBeginPath(vg_);
  nvgRoundedRect(vg_, x, y, w, h, radius);
  nvgFillColor(vg_, col(color));
  nvgFill(vg_);
}

void Canvas::strokeRoundRect(float x, float y, float w, float h, float radius, float width,
                             uint32_t color) {
  nvgBeginPath(vg_);
  nvgRoundedRect(vg_, x, y, w, h, radius);
  nvgStrokeColor(vg_, col(color));
  nvgStrokeWidth(vg_, width);
  nvgStroke(vg_);
}

void Canvas::fillCircle(float cx, float cy, float r, uint32_t color) {
  nvgBeginPath(vg_);
  nvgCircle(vg_, cx, cy, r);
  nvgFillColor(vg_, col(color));
  nvgFill(vg_);
}

void Canvas::strokeCircle(float cx, float cy, float r, float width, uint32_t color) {
  nvgBeginPath(vg_);
  nvgCircle(vg_, cx, cy, r);
  nvgStrokeColor(vg_, col(color));
  nvgStrokeWidth(vg_, width);
  nvgStroke(vg_);
}

void Canvas::fillCircleGradient(float cx, float cy, float r, uint32_t inner, uint32_t outer,
                                float ox, float oy) {
  nvgBeginPath(vg_);
  nvgCircle(vg_, cx, cy, r);
  nvgFillPaint(vg_, nvgRadialGradient(vg_, cx + ox * r, cy + oy * r, r * 0.05f, r * 1.35f,
                                      col(inner), col(outer)));
  nvgFill(vg_);
}

void Canvas::line(float x0, float y0, float x1, float y1, float width, uint32_t color) {
  nvgBeginPath(vg_);
  nvgMoveTo(vg_, x0, y0);
  nvgLineTo(vg_, x1, y1);
  nvgLineCap(vg_, NVG_ROUND);
  nvgStrokeColor(vg_, col(color));
  nvgStrokeWidth(vg_, width);
  nvgStroke(vg_);
}

void Canvas::polyline(const float* xy, int n, float width, uint32_t color) {
  if (n < 2) return;
  nvgBeginPath(vg_);
  nvgMoveTo(vg_, xy[0], xy[1]);
  for (int i = 1; i < n; ++i) nvgLineTo(vg_, xy[2 * i], xy[2 * i + 1]);
  nvgLineJoin(vg_, NVG_ROUND);
  nvgStrokeColor(vg_, col(color));
  nvgStrokeWidth(vg_, width);
  nvgStroke(vg_);
}

float Canvas::text(float x, float y, const std::string& s, float size, uint32_t color, int align,
                   bool middle) {
  if (font_ < 0 || s.empty()) return 0.f;
  nvgFontFaceId(vg_, font_);
  nvgFontSize(vg_, size * kEmToFontSize);
  nvgTextLetterSpacing(vg_, 0.f);
  int h = align == 1 ? NVG_ALIGN_CENTER : (align == 2 ? NVG_ALIGN_RIGHT : NVG_ALIGN_LEFT);
  nvgTextAlign(vg_, h | (middle ? NVG_ALIGN_MIDDLE : NVG_ALIGN_BASELINE));
  nvgFillColor(vg_, col(color));
  return nvgText(vg_, x, y, s.c_str(), NULL) - x;
}

float Canvas::textWidth(const std::string& s, float size) {
  if (font_ < 0 || s.empty()) return 0.f;
  nvgFontFaceId(vg_, font_);
  nvgFontSize(vg_, size * kEmToFontSize);
  nvgTextLetterSpacing(vg_, 0.f);
  nvgTextAlign(vg_, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
  return nvgTextBounds(vg_, 0.f, 0.f, s.c_str(), NULL, NULL);
}

}  // namespace br
