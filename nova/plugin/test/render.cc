// Nova Modular - GPL-3.0-or-later.
//
// Offline check of the wrapped modules, without Rack: builds each BrickRack
// module, patches a few of them together, renders a couple of seconds at
// 48 kHz and prints what came out (level, pitch, and the tests specific to
// each module). It answers "does the firmware DSP still do its job in this
// tree, built with this compiler" in a second, on the computer.
//
//   nova/plugin/test/run.sh          (builds this file and runs it)
//
// With a directory as argument it also writes each test's output there as a
// 16-bit WAV, to listen to.

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/model.h"
#include "core/module.h"
#include "ui/canvas.h"

// The modules' panel drawing is never called here; it only has to link.
namespace br {
float Canvas::text(float, float, const std::string&, float, uint32_t, int, bool) { return 0.f; }
void Canvas::strokeCircle(float, float, float, float, uint32_t) {}
}  // namespace br

namespace {

const float kRate = 48000.f;
int failures = 0;
std::string wavDir;

std::unique_ptr<br::Module> make(const char* slug) {
  const br::Model* model = br::findModel(slug);
  if (!model) {
    std::printf("FAIL  no model %s\n", slug);
    ++failures;
    std::exit(1);
  }
  std::unique_ptr<br::Module> m(model->create());
  m->model = model;
  m->onSampleRateChange(kRate);
  return m;
}

int64_t frame = 0;
void step(br::Module& m) {
  br::ProcessArgs a;
  a.sampleRate = kRate;
  a.sampleTime = 1.f / kRate;
  a.engine = nullptr;
  a.frame = frame;
  m.process(a);
}

void patch(br::Module& m, int input, float v) {
  m.inputs[static_cast<size_t>(input)].connected = true;
  m.inputs[static_cast<size_t>(input)].voltage = v;
}

float rms(const std::vector<float>& x, size_t from = 0) {
  double s = 0;
  for (size_t i = from; i < x.size(); ++i) s += static_cast<double>(x[i]) * x[i];
  return x.size() > from ? static_cast<float>(std::sqrt(s / static_cast<double>(x.size() - from))) : 0.f;
}

/// Fundamental by autocorrelation, 40 Hz .. 2 kHz, on the last 8192 samples.
float pitch(const std::vector<float>& x) {
  const size_t n = 8192;
  if (x.size() < n) return 0.f;
  const float* p = &x[x.size() - n];
  double mean = 0;
  for (size_t i = 0; i < n; ++i) mean += p[i];
  mean /= n;
  int best = 0;
  double bestV = 0, zero = 0;
  for (size_t i = 0; i < n; ++i) zero += (p[i] - mean) * (p[i] - mean);
  const int lo = static_cast<int>(kRate / 2000.f), hi = static_cast<int>(kRate / 40.f);
  std::vector<double> ac(static_cast<size_t>(hi + 2));
  for (int lag = lo - 1; lag <= hi + 1; ++lag) {
    double s = 0;
    for (size_t i = 0; i + static_cast<size_t>(lag) < n; ++i) s += (p[i] - mean) * (p[i + lag] - mean);
    ac[static_cast<size_t>(lag)] = s / static_cast<double>(n - static_cast<size_t>(lag));
  }
  // First strong peak, so a harmonic-rich wave does not report an octave down.
  for (int lag = lo; lag <= hi; ++lag) {
    double v = ac[static_cast<size_t>(lag)];
    if (v > ac[static_cast<size_t>(lag - 1)] && v >= ac[static_cast<size_t>(lag + 1)] &&
        v > 0.8 * zero / n && v > bestV) {
      bestV = v;
      best = lag;
      break;
    }
  }
  return best ? kRate / static_cast<float>(best) : 0.f;
}

/// Share of the energy above `hz`, by a one-pole split. Rough, but enough to
/// tell a closed filter from an open one.
float brightness(const std::vector<float>& x, float hz) {
  float a = std::exp(-2.f * static_cast<float>(M_PI) * hz / kRate), lp = 0.f;
  double lo = 0, hiE = 0;
  for (float v : x) {
    lp = a * lp + (1.f - a) * v;
    lo += static_cast<double>(lp) * lp;
    hiE += static_cast<double>(v - lp) * (v - lp);
  }
  return lo + hiE > 0 ? static_cast<float>(hiE / (lo + hiE)) : 0.f;
}

void writeWav(const std::string& name, const std::vector<float>& x, float fullScale) {
  if (wavDir.empty()) return;
  std::string path = wavDir + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  uint32_t dataBytes = static_cast<uint32_t>(x.size() * 2), rate = 48000;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  std::fwrite("data", 1, 4, f); u32(dataBytes);
  for (float v : x) {
    float s = std::max(-1.f, std::min(1.f, v / fullScale));
    int16_t i = static_cast<int16_t>(s * 32767.f);
    std::fwrite(&i, 2, 1, f);
  }
  std::fclose(f);
}

void check(const char* what, bool ok, const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  std::printf("%s  %-34s %s\n", ok ? "ok  " : "FAIL", what, buf);
  if (!ok) ++failures;
}

bool finite(const std::vector<float>& x) {
  for (float v : x) if (!std::isfinite(v)) return false;
  return true;
}

// --- Plaits ----------------------------------------------------------------

void testPlaits() {
  // Every model must make sound, and the pitched ones must follow V/OCT.
  enum { FREQ, HARMONICS, TIMBRE, MORPH, TIMBRE_CV, FREQ_CV, MORPH_CV, MODEL1, MODEL2, ENGINE };
  enum { IN_ENGINE, IN_TIMBRE, IN_FREQ, IN_MORPH, IN_HARMONICS, IN_TRIGGER, IN_LEVEL, IN_NOTE };
  int silent = 0, bad = 0;
  for (int engine = 0; engine < 24; ++engine) {
    auto m = make("macro-osc-2");
    m->params[ENGINE].set(static_cast<float>(engine));
    std::vector<float> out;
    for (int i = 0; i < 48000; ++i) {
      // Gated twice a second, a quarter of a second each time: the drum
      // models (21-23) need the edge, and a 6-op FM patch with a slow attack
      // needs the gate held to be heard at all.
      patch(*m, IN_TRIGGER, (i % 24000) < 12000 ? 8.f : 0.f);
      step(*m);
      out.push_back(m->outputs[0].voltage);
    }
    if (!finite(out)) ++bad;
    if (rms(out) < 0.01f) {
      ++silent;
      std::printf("      model %d is silent (rms %.4f)\n", engine, rms(out));
    }
    if (engine == 8) writeWav("plaits-virtual-analog", out, 8.f);
    if (engine == 2) writeWav("plaits-6op-fm", out, 8.f);
  }
  check("Plaits: 24 models sound", silent == 0 && bad == 0, "%d silent, %d not finite", silent, bad);

  // Virtual analog (model 8), untriggered: C4 at 0 V, C5 at 1 V.
  for (int oct = 0; oct < 2; ++oct) {
    auto m = make("macro-osc-2");
    m->params[ENGINE].set(8.f);
    std::vector<float> out;
    for (int i = 0; i < 48000; ++i) {
      patch(*m, IN_NOTE, static_cast<float>(oct));
      step(*m);
      out.push_back(m->outputs[0].voltage);
    }
    float f = pitch(out), want = 261.63f * static_cast<float>(1 << oct);
    check(oct ? "Plaits: 1 V is C5" : "Plaits: 0 V is C4", std::fabs(f - want) < want * 0.01f,
          "%.1f Hz (want %.1f), rms %.2f V", f, want, rms(out));
  }
}

// --- Edges -----------------------------------------------------------------

float baseEdges = 0.f;

void testEdges() {
  enum { GATE1 = 0, VOCT1 = 4 };
  enum { OUT1 = 0, MIX = 4 };
  for (int oct = 0; oct < 2; ++oct) {
    auto m = make("chiptune-osc");
    std::vector<float> out, mix;
    for (int i = 0; i < 48000; ++i) {
      patch(*m, GATE1, 8.f);
      patch(*m, VOCT1, static_cast<float>(oct));
      step(*m);
      out.push_back(m->outputs[OUT1].voltage);
      mix.push_back(m->outputs[MIX].voltage);
    }
    float f = pitch(out);
    if (oct == 0) {
      baseEdges = f;
      check("Edges: channel 1 sounds", finite(out) && rms(out) > 0.1f && f > 20.f, "%.1f Hz, rms %.2f V, mix rms %.2f V",
            f, rms(out), rms(mix));
      writeWav("edges-channel-1", out, 8.f);
    } else {
      check("Edges: +1 V is an octave up", std::fabs(f - 2.f * baseEdges) < 2.f * baseEdges * 0.02f,
            "%.1f Hz (want %.1f)", f, 2.f * baseEdges);
    }
  }
}
// --- Peaks -----------------------------------------------------------------

void testPeaks() {
  // Default function: envelope. A gate must raise the output and its end
  // must let it fall again.
  enum { GATE1 = 0 };
  auto m = make("dual-trigger");
  std::vector<float> out;
  float peak = 0.f, atEnd = 0.f;
  for (int i = 0; i < 96000; ++i) {
    patch(*m, GATE1, (i >= 4800 && i < 28800) ? 8.f : 0.f);
    step(*m);
    float v = m->outputs[0].voltage;
    out.push_back(v);
    peak = std::max(peak, std::fabs(v));
    atEnd = v;
  }
  float before = 0.f;
  for (int i = 0; i < 4000; ++i) before = std::max(before, std::fabs(out[static_cast<size_t>(i)]));
  check("Peaks: envelope follows the gate", finite(out) && peak > 4.f && before < 0.5f && std::fabs(atEnd) < 0.5f,
        "rest %.2f V, peak %.2f V, after release %.2f V", before, peak, atEnd);
  writeWav("peaks-envelope", out, 10.f);
}

// --- Blades ----------------------------------------------------------------

void testBlades() {
  // White noise through filter 1 (low-pass by default): closing the cutoff
  // must remove the top, opening it must let it through.
  enum { FREQ1 = 0 };
  enum { IN1 = 0 };
  enum { OUT1 = 0, MAIN = 2 };
  float bright[2] = {0, 0}, level[2] = {0, 0};
  for (int k = 0; k < 2; ++k) {
    auto m = make("dual-multimode-filter");
    m->params[FREQ1].set(k == 0 ? 0.3f : 0.95f);
    uint32_t seed = 12345;
    std::vector<float> out;
    for (int i = 0; i < 48000; ++i) {
      seed = seed * 1664525u + 1013904223u;
      float noise = (static_cast<float>(seed >> 8) / 8388608.f - 1.f) * 3.f;
      patch(*m, IN1, noise);
      step(*m);
      out.push_back(m->outputs[OUT1].voltage);
    }
    bright[k] = brightness(out, 2000.f);
    level[k] = rms(out, 4800);
    if (!finite(out)) level[k] = -1.f;
    writeWav(k == 0 ? "blades-closed" : "blades-open", out, 8.f);
  }
  check("Blades: cutoff shapes the noise", level[0] > 0.01f && level[1] > 0.01f && bright[1] > bright[0] * 3.f,
        "energy above 2 kHz: closed %.1f%%, open %.1f%% (rms %.2f / %.2f V)",
        bright[0] * 100.f, bright[1] * 100.f, level[0], level[1]);
}

// --- Beads -----------------------------------------------------------------

void testBeads() {
  // A sawtooth in, fully wet, density turned up: grains must come out.
  enum { DENSITY, TIME, PITCH, SIZE, SHAPE, FEEDBACK, DRY_WET, REVERB };
  enum { IN_L = 6, IN_R = 7 };
  auto m = make("texture-synth-2");
  m->params[DENSITY].set(0.85f);
  m->params[DRY_WET].set(1.f);
  m->params[SIZE].set(0.7f);
  std::vector<float> outL, outR;
  float phase = 0.f;
  for (int i = 0; i < 48000 * 4; ++i) {
    phase += 220.f / kRate;
    if (phase >= 1.f) phase -= 1.f;
    float saw = (phase * 2.f - 1.f) * 5.f;
    patch(*m, IN_L, saw);
    patch(*m, IN_R, saw);
    step(*m);
    outL.push_back(m->outputs[0].voltage);
    outR.push_back(m->outputs[1].voltage);
  }
  float l = rms(outL, 48000), r = rms(outR, 48000);
  check("Beads: grains from a sawtooth", finite(outL) && finite(outR) && l > 0.05f && r > 0.05f,
        "wet rms L %.2f V, R %.2f V", l, r);
  writeWav("beads-grains", outL, 10.f);

  // Nothing patched in: after ten seconds it turns into its wavetable synth.
  auto s = make("texture-synth-2");
  s->params[DENSITY].set(0.85f);
  s->params[DRY_WET].set(1.f);
  std::vector<float> synth;
  for (int i = 0; i < 48000 * 14; ++i) {
    step(*s);
    if (i >= 48000 * 12) synth.push_back(s->outputs[0].voltage);
  }
  check("Beads: wavetable synth when unpatched", finite(synth) && rms(synth) > 0.02f, "rms %.2f V", rms(synth));
  writeWav("beads-synth", synth, 10.f);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1) wavDir = argv[1];
  testPlaits();
  testEdges();
  testPeaks();
  testBlades();
  testBeads();
  std::printf(failures ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", failures);
  return failures ? 1 : 0;
}
