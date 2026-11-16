// Nova Modular - GPL-3.0-or-later.
//
// See adapter.hpp.

#include "adapter.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
#include <vector>

#include "core/model.h"
#include "core/module.h"
#include "ui/canvas.h"

namespace {

const char* const kFontPath = "res/fonts/BarlowCondensed-SemiBold.ttf";

/** Pixels per millimetre on a Rack panel (128.5 mm = 380 px). */
const float kMm = 75.f / 25.4f;

/** The scale handed to the modules' own drawing code. They clamp text to a
readable size on BrickRack's screen ("at least 12 px"), which at Rack's
2.95 px/mm would always win and blow the text up; at 6 px/mm their
proportional sizes win, and the context is scaled down to match. */
const float kDrawScale = 6.f;

NVGcolor nvgArgb(uint32_t c) {
	return nvgRGBA((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF, (c >> 24) & 0xFF);
}

int fontHandle() {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::plugin(pluginInstance, kFontPath));
	return font ? font->handle : -1;
}

/** One instance per model for drawing a panel that has no module behind it
(the module browser, screenshots): knobs sit at their defaults and the
panels' read-outs have something to read. Main thread only. */
const br::Module* previewModule(const br::Model* model) {
	static std::map<const br::Model*, std::unique_ptr<br::Module>> cache;
	std::unique_ptr<br::Module>& m = cache[model];
	if (!m) {
		m.reset(model->create());
		m->model = model;
	}
	return m.get();
}

// ---------------------------------------------------------------------------
// Module

struct NovaModule : engine::Module {
	static const int kMaxSrcChannels = 32;
	/** Frames rendered at the native rate per conversion. Plaits' own block. */
	static const int kBlock = 12;

	const br::Model* brModel;
	NovaModelOptions opt;
	std::vector<std::unique_ptr<br::Module>> voices;
	/** The value each param had when the two sides last agreed. Whichever
	side then differs is the one that changed: the user turned a knob, or the
	module set one of its own params (a button that steps a hidden setting). */
	std::vector<float> agreed;
	int64_t frame = 0;
	int lightDivider = 0;

	bool resample = false;
	dsp::SampleRateConverter<kMaxSrcChannels> src;
	dsp::DoubleRingBuffer<dsp::Frame<kMaxSrcChannels>, 256> outBuf;

	NovaModule(const br::Model* model, const NovaModelOptions& options) : brModel(model), opt(options) {
		int numVoices = std::max(1, opt.maxVoices);
		for (int i = 0; i < numVoices; ++i) {
			br::Module* m = model->create();
			m->model = model;
			m->id = i;
			voices.emplace_back(m);
		}
		br::Module* v = voices[0].get();
		config((int) v->params.size(), (int) v->inputs.size(), (int) v->outputs.size(), (int) v->lights.size());

		for (size_t i = 0; i < v->params.size(); ++i) {
			const br::Param& p = v->params[i];
			engine::ParamQuantity* pq = NULL;
			switch (p.type) {
				case br::ParamType::Switch:
					pq = configSwitch((int) i, p.minValue, p.maxValue, p.defaultValue, p.name, p.labels);
					break;
				case br::ParamType::Button:
					pq = configButton((int) i, p.name);
					break;
				case br::ParamType::Toggle:
					pq = configSwitch((int) i, 0.f, 1.f, p.defaultValue, p.name, {"Off", "On"});
					break;
				default: {
					// Rack prints the unit right after the number.
					std::string unit = p.unit.empty() || p.unit == "%" ? p.unit : " " + p.unit;
					pq = configParam((int) i, p.minValue, p.maxValue, p.defaultValue, p.name, unit,
						p.displayBase, p.displayMultiplier, p.displayOffset);
					pq->snapEnabled = p.snap;
					break;
				}
			}
			// A setting reached from the menu is not something a "randomize"
			// should touch (it would change Plaits' model, Beads' quality...).
			if (p.hidden)
				pq->randomizeEnabled = false;
			agreed.push_back(p.value);
		}
		for (size_t i = 0; i < v->inputs.size(); ++i)
			configInput((int) i, v->inputs[i].name);
		for (size_t i = 0; i < v->outputs.size(); ++i)
			configOutput((int) i, v->outputs[i].name);

		setRate(APP->engine->getSampleRate());
	}

	void setRate(float sampleRate) {
		float rate = sampleRate;
		resample = opt.nativeRate > 0.f && std::fabs(sampleRate - opt.nativeRate) > 0.5f;
		if (resample) {
			rate = opt.nativeRate;
			src.setRates((int) std::lround(opt.nativeRate), (int) std::lround(sampleRate));
			outBuf.clear();
		}
		for (auto& v : voices)
			v->onSampleRateChange(rate);
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		setRate(e.sampleRate);
	}

	/** Hands every voice the params as Rack has them. */
	void pushParams() {
		for (size_t i = 0; i < params.size(); ++i) {
			agreed[i] = params[i].getValue();
			for (auto& v : voices)
				v->params[i].value = agreed[i];
		}
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		pushParams();
		for (auto& v : voices)
			v->onReset();
	}

	json_t* dataToJson() override {
		br::StateMap state;
		voices[0]->saveState(state);
		json_t* stateJ = json_object();
		for (const auto& kv : state)
			json_object_set_new(stateJ, kv.first.c_str(), json_string(kv.second.c_str()));
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "state", stateJ);
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		br::StateMap state;
		json_t* stateJ = json_object_get(rootJ, "state");
		const char* key;
		json_t* value;
		json_object_foreach(stateJ, key, value) {
			if (json_is_string(value))
				state[key] = json_string_value(value);
		}
		// The modules read their params while loading their state, and Rack
		// has restored those by now.
		pushParams();
		for (auto& v : voices)
			v->loadState(state);
	}

	int channelCount() {
		if (voices.size() <= 1)
			return 1;
		int channels = 1;
		for (engine::Input& in : inputs)
			channels = std::max(channels, in.getChannels());
		return std::min(channels, (int) voices.size());
	}

	/** One frame of the wrapped module, every voice of it. */
	void stepVoices(float rate, int channels) {
		br::Module* v0 = voices[0].get();
		const int numParams = (int) params.size();
		for (int i = 0; i < numParams; ++i) {
			float value = params[i].getValue();
			if (value != agreed[i]) {
				agreed[i] = value;
				v0->params[i].value = value;
			}
		}
		br::ProcessArgs args;
		args.sampleRate = rate;
		args.sampleTime = 1.f / rate;
		args.engine = nullptr;
		args.frame = frame++;
		const int numInputs = (int) inputs.size();
		const int numOutputs = (int) outputs.size();
		for (int c = 0; c < channels; ++c) {
			br::Module* v = voices[c].get();
			if (c > 0) {
				// The panel is shared: every voice follows the first one.
				for (int i = 0; i < numParams; ++i)
					v->params[i].value = v0->params[i].value;
			}
			for (int i = 0; i < numInputs; ++i) {
				v->inputs[i].connected = inputs[i].isConnected();
				v->inputs[i].voltage = inputs[i].getPolyVoltage(c);
			}
			for (int i = 0; i < numOutputs; ++i)
				v->outputs[i].connected = outputs[i].isConnected();
			v->process(args);
			if (c == 0) {
				for (int i = 0; i < numParams; ++i) {
					float value = v0->params[i].value;
					if (value != agreed[i]) {
						agreed[i] = value;
						params[i].setValue(value);
					}
				}
			}
		}
	}

	static float clean(float v) {
		return std::isfinite(v) ? v : 0.f;
	}

	void process(const ProcessArgs& args) override {
		const int channels = channelCount();
		const int numOutputs = (int) outputs.size();

		if (!resample) {
			stepVoices(args.sampleRate, channels);
			for (int o = 0; o < numOutputs; ++o) {
				outputs[o].setChannels(channels);
				for (int c = 0; c < channels; ++c)
					outputs[o].setVoltage(clean(voices[c]->outputs[o].voltage), c);
			}
		}
		else {
			if (outBuf.empty()) {
				dsp::Frame<kMaxSrcChannels> in[kBlock] = {};
				for (int i = 0; i < kBlock; ++i) {
					stepVoices(opt.nativeRate, channels);
					for (int c = 0; c < channels; ++c) {
						for (int o = 0; o < numOutputs; ++o)
							in[i].samples[c * numOutputs + o] = clean(voices[c]->outputs[o].voltage);
					}
				}
				src.setChannels(std::min(channels * numOutputs, (int) kMaxSrcChannels));
				int inFrames = kBlock;
				int outFrames = (int) outBuf.capacity();
				src.process(in, &inFrames, outBuf.endData(), &outFrames);
				outBuf.endIncr(outFrames);
			}
			if (!outBuf.empty()) {
				dsp::Frame<kMaxSrcChannels> f = outBuf.shift();
				for (int o = 0; o < numOutputs; ++o) {
					outputs[o].setChannels(channels);
					for (int c = 0; c < channels; ++c)
						outputs[o].setVoltage(f.samples[c * numOutputs + o], c);
				}
			}
		}

		if ((lightDivider++ & 15) == 0) {
			const br::Module* v0 = voices[0].get();
			for (size_t i = 0; i < lights.size(); ++i)
				lights[i].setBrightness(v0->lights[i]);
		}
	}
};

// ---------------------------------------------------------------------------
// Panel

/** Everything on the panel that never changes, drawn once into the
framebuffer it sits in: the plate, the printing, and the bodies of the knobs
(only their pointers move). */
struct NovaPanel : widget::Widget {
	const br::PanelSpec* spec = NULL;

	void draw(const DrawArgs& args) override {
		br::Canvas c(args.vg, fontHandle());
		const br::PanelSpec& p = *spec;
		const float s = kMm;
		const float W = box.size.x, H = box.size.y;

		c.fillRect(0, 0, W, H, p.bg);
		// Subtle bevel.
		c.fillRect(0, 0, W, 1.f, br::scaleColor(p.bg, 1.08f));
		c.fillRect(0, 0, 1.f, H, br::scaleColor(p.bg, 1.06f));
		c.fillRect(W - 1.f, 0, 1.f, H, br::scaleColor(p.bg, 0.72f));
		c.fillRect(0, H - 1.f, W, 1.f, br::scaleColor(p.bg, 0.78f));

		auto screw = [&](float x, float y) {
			c.fillCircleGradient(x * s, y * s, 1.6f * s, 0xFFE4E6E8, 0xFF8C9096, -0.3f, -0.3f);
			c.line((x - 1.0f) * s, y * s, (x + 1.0f) * s, y * s, 0.35f * s, 0xFF55595F);
		};
		const float widthMm = p.widthMm();
		float sx = p.hp >= 10 ? 7.5f : widthMm * 0.5f;
		screw(sx, 3.0f);
		screw(widthMm - sx, br::kPanelHeightMm - 3.0f);
		if (p.hp >= 10) {
			screw(widthMm - sx, 3.0f);
			screw(sx, br::kPanelHeightMm - 3.0f);
		}

		for (const br::Widget& w : p.widgets) {
			uint32_t ink = w.color ? w.color : p.ink;
			if (w.type == br::WidgetType::Box)
				c.fillRoundRect(w.x * s, w.y * s, w.w * s, w.h * s, w.x2 * s, w.color);
			else if (w.type == br::WidgetType::Line)
				c.line(w.x * s, w.y * s, w.x2 * s, w.y2 * s, std::max(0.6f, w.w * s), ink);
		}
		for (const br::Widget& w : p.widgets) {
			uint32_t ink = w.color ? w.color : p.ink;
			if (w.type == br::WidgetType::Label) {
				c.text(w.x * s, w.y * s, w.text, w.fontSize * s, ink, w.align);
			}
			else if (w.type == br::WidgetType::Knob) {
				float cx = w.x * s, cy = w.y * s, r = w.w * 0.5f * s;
				bool small = w.w < 9.f;
				uint32_t body = w.color ? w.color : (small ? 0xFFB9BDC3 : 0xFF2E3034);
				uint32_t hi = br::scaleColor(body, small ? 1.18f : 2.1f);
				uint32_t lo = br::scaleColor(body, small ? 0.62f : 0.55f);
				c.fillCircle(cx + 0.06f * r, cy + 0.12f * r, r * 1.04f, 0x50000000);
				c.fillCircleGradient(cx, cy, r, hi, lo, -0.35f, -0.45f);
				c.strokeCircle(cx, cy, r * 0.97f, std::max(0.6f, r * 0.05f), br::scaleColor(body, 0.5f));
				if (!small) {
					c.fillCircleGradient(cx, cy, r * 0.72f, br::scaleColor(body, 1.5f),
						br::scaleColor(body, 0.8f), -0.3f, -0.4f);
				}
			}
		}

		if (!p.title.empty()) {
			float size = 3.5f * s;
			size_t nl = p.title.find('\n');
			if (nl == std::string::npos) {
				c.text(W * 0.5f, 7.6f * s, p.title, size, p.ink, br::kAlignCenter);
			}
			else {
				float small = size * 0.82f;
				c.text(W * 0.5f, 6.0f * s, p.title.substr(0, nl), small, p.ink, br::kAlignCenter);
				c.text(W * 0.5f, 6.0f * s + small * 1.05f, p.title.substr(nl + 1), small, p.ink, br::kAlignCenter);
			}
		}
	}
};

struct NovaKnob : app::Knob {
	bool small = false;
	/** Where the pointer rests with no module behind it, 0..1. */
	float previewValue = 0.5f;

	NovaKnob() {
		minAngle = -0.75f * M_PI;
		maxAngle = 0.75f * M_PI;
	}

	void draw(const DrawArgs& args) override {
		float v = previewValue;
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq) {
			float value = pq->getValue();
			if (pq->snapEnabled)
				value = std::round(value);
			float lo = pq->getMinValue(), hi = pq->getMaxValue();
			v = hi > lo ? (value - lo) / (hi - lo) : 0.f;
		}
		const float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f, r = box.size.x * 0.5f;
		const float a = (0.75f + 1.5f * math::clamp(v, 0.f, 1.f)) * M_PI;
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, cx + std::cos(a) * r * 0.18f, cy + std::sin(a) * r * 0.18f);
		nvgLineTo(args.vg, cx + std::cos(a) * r * 0.86f, cy + std::sin(a) * r * 0.86f);
		nvgLineCap(args.vg, NVG_ROUND);
		nvgStrokeWidth(args.vg, std::max(1.2f, r * 0.13f));
		nvgStrokeColor(args.vg, nvgArgb(small ? 0xFF1E1F22 : 0xFFF4F4F4));
		nvgStroke(args.vg);
	}
};

/** A round push button, momentary or latching, with an optional light in it. */
struct NovaButton : app::Switch {
	uint32_t color = 0xFFCDD0D4;
	uint32_t lightColor = 0xFFFFE9A8;
	int lightId = -1;

	float brightness() {
		if (!module || lightId < 0 || lightId >= (int) module->lights.size())
			return 0.f;
		return math::clamp(module->lights[lightId].getBrightness(), 0.f, 1.f);
	}

	void draw(const DrawArgs& args) override {
		engine::ParamQuantity* pq = getParamQuantity();
		bool down = pq && pq->getValue() > 0.5f;
		br::Canvas c(args.vg, -1);
		const float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f, r = box.size.x * 0.5f;
		c.fillCircle(cx, cy + 0.6f, r * 1.12f, 0x50000000);
		c.fillCircleGradient(cx, cy, r, br::scaleColor(color, down ? 0.85f : 1.15f),
			br::scaleColor(color, down ? 0.55f : 0.7f), -0.3f, -0.4f);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			float b = brightness();
			if (b > 0.01f) {
				br::Canvas c(args.vg, -1);
				c.fillCircle(box.size.x * 0.5f, box.size.y * 0.5f, box.size.x * 0.35f, br::withAlpha(lightColor, b));
			}
		}
		Widget::drawLayer(args, layer);
	}
};

/** A small vertical toggle with two or three positions. */
struct NovaToggle : app::Switch {
	void draw(const DrawArgs& args) override {
		br::Canvas c(args.vg, -1);
		const float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f;
		const float hw = box.size.x * 0.5f, hh = box.size.y * 0.5f;
		c.fillRoundRect(cx - hw, cy - hh, 2 * hw, 2 * hh, hw * 0.5f, 0xFF2A2C30);
		float t = 0.f;
		engine::ParamQuantity* pq = getParamQuantity();
		if (pq && pq->getMaxValue() > pq->getMinValue())
			t = (std::round(pq->getValue()) - pq->getMinValue()) / (pq->getMaxValue() - pq->getMinValue());
		float ly = cy + hh * 0.55f - t * hh * 1.1f;
		c.fillRoundRect(cx - hw * 0.8f, ly - hh * 0.38f, hw * 1.6f, hh * 0.76f, hw * 0.4f, 0xFFE3E5E8);
	}
};

/** An LED: one colour, two colours mixed, or RGB. */
struct NovaLight : widget::TransparentWidget {
	engine::Module* module = NULL;
	int a = -1, b = -1, c3 = -1;
	uint32_t colorA = 0xFFFF3030, colorB = 0;

	float level(int i) {
		if (!module || i < 0 || i >= (int) module->lights.size())
			return 0.f;
		return math::clamp(module->lights[i].getBrightness(), 0.f, 1.f);
	}

	/** Same rules as BrickRack's own drawing. */
	uint32_t current(float* bright) {
		if (c3 == -2) {
			// Two dies in one package: red + green = yellow.
			float la = level(a), lb = level(b);
			*bright = std::max(la, lb);
			if (*bright <= 0.f)
				return colorA;
			auto ch = [&](int sh) {
				float v = (((colorA >> sh) & 0xFF) * la + ((colorB >> sh) & 0xFF) * lb) / *bright;
				return std::min(255, (int) v);
			};
			return br::argb(255, ch(16), ch(8), ch(0));
		}
		if (b >= 0) {
			float r = level(a), g = level(b), bl = level(c3);
			*bright = std::max(r, std::max(g, bl));
			if (*bright <= 0.f)
				return 0xFF808080;
			return br::argb(255, (int) (255 * r / *bright), (int) (255 * g / *bright), (int) (255 * bl / *bright));
		}
		*bright = level(a);
		return colorA;
	}

	void draw(const DrawArgs& args) override {
		br::Canvas c(args.vg, -1);
		const float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f, r = box.size.x * 0.5f;
		float bright;
		uint32_t col = current(&bright);
		c.fillCircle(cx, cy, r * 1.1f, 0xFF1A1A1C);
		c.fillCircle(cx, cy, r * 0.9f, br::mixColor(0xFF3A3A3D, col, 0.18f));
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			float bright;
			uint32_t col = current(&bright);
			if (bright > 0.02f) {
				br::Canvas c(args.vg, -1);
				const float cx = box.size.x * 0.5f, cy = box.size.y * 0.5f, r = box.size.x * 0.5f;
				c.fillCircle(cx, cy, r * 0.9f, br::withAlpha(col, bright));
				c.fillCircle(cx, cy, r * 2.0f, br::withAlpha(col, 0.22f * bright));
				c.fillCircle(cx - r * 0.25f, cy - r * 0.25f, r * 0.35f, br::withAlpha(0xFFFFFFFF, 0.5f * bright));
			}
		}
		Widget::drawLayer(args, layer);
	}
};

/** An area a module draws itself: the name of Plaits' model, and the like. */
struct NovaDisplay : widget::TransparentWidget {
	const br::Model* brModel = NULL;
	NovaModule* module = NULL;
	br::CustomDraw drawFn;

	void draw(const DrawArgs& args) override {
		if (!drawFn)
			return;
		const br::Module* m = module ? module->voices[0].get() : previewModule(brModel);
		br::Canvas c(args.vg, fontHandle());
		const float k = kMm / kDrawScale;
		nvgSave(args.vg);
		nvgScale(args.vg, k, k);
		drawFn(c, 0.f, 0.f, box.size.x / k, box.size.y / k, kDrawScale, m);
		nvgRestore(args.vg);
	}
};

/** A value slider for a setting that has no knob. Desktop Rack shows it in
the menu; RackDroid's menus only carry items, so the steps beside it are what
a touch screen or a controller uses. */
struct NovaMenuSlider : ui::Slider {
	NovaMenuSlider(engine::ParamQuantity* pq) {
		quantity = pq;
		box.size.x = 220.f;
	}
};

std::string stepLabel(engine::ParamQuantity* pq, float value) {
	float shown = value;
	if (pq->displayBase > 0.f)
		shown = std::pow(pq->displayBase, value);
	shown = shown * pq->displayMultiplier + pq->displayOffset;
	std::string text = std::fabs(shown - std::round(shown)) < 1e-3f
		? string::f("%d", (int) std::lround(shown)) : string::f("%.3g", shown);
	return text + pq->unit;
}

struct NovaWidget : app::ModuleWidget {
	NovaWidget(NovaModule* module, const br::Model* brModel) {
		setModule(module);
		const br::PanelSpec& p = brModel->panel();
		box.size = math::Vec(RACK_GRID_WIDTH * p.hp, RACK_GRID_HEIGHT);

		widget::FramebufferWidget* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		NovaPanel* panel = new NovaPanel;
		panel->spec = &p;
		panel->box.size = box.size;
		fb->addChild(panel);
		addChild(fb);

		const br::Module* preview = module ? NULL : previewModule(brModel);

		for (const br::Widget& w : p.widgets) {
			const math::Vec centre = math::Vec(w.x, w.y).mult(kMm);
			const math::Vec size = math::Vec(w.w, w.h).mult(kMm);
			switch (w.type) {
				case br::WidgetType::Knob: {
					NovaKnob* k = createParamCentered<NovaKnob>(centre, module, w.index);
					k->box.size = size;
					k->box.pos = centre.minus(size.div(2));
					k->small = w.w < 9.f;
					if (preview)
						k->previewValue = preview->params[w.index].normalized();
					addParam(k);
					break;
				}
				case br::WidgetType::Button: {
					NovaButton* b = createParamCentered<NovaButton>(centre, module, w.index);
					b->box.size = size;
					b->box.pos = centre.minus(size.div(2));
					const br::Param& bp = (module ? module->voices[0].get() : preview)->params[w.index];
					b->momentary = bp.type == br::ParamType::Button;
					if (w.color)
						b->color = w.color;
					if (w.color2)
						b->lightColor = w.color2;
					b->lightId = w.index2;
					addParam(b);
					break;
				}
				case br::WidgetType::Switch: {
					NovaToggle* t = createParamCentered<NovaToggle>(centre, module, w.index);
					t->box.size = size;
					t->box.pos = centre.minus(size.div(2));
					addParam(t);
					break;
				}
				case br::WidgetType::Input:
					addInput(createInputCentered<componentlibrary::PJ301MPort>(centre, module, w.index));
					break;
				case br::WidgetType::Output:
					addOutput(createOutputCentered<componentlibrary::PJ301MPort>(centre, module, w.index));
					break;
				case br::WidgetType::Light: {
					NovaLight* l = new NovaLight;
					l->module = module;
					l->box.size = size;
					l->box.pos = centre.minus(size.div(2));
					l->a = w.index;
					l->b = w.index2;
					l->c3 = w.index3;
					if (w.color)
						l->colorA = w.color;
					l->colorB = w.color2;
					addChild(l);
					break;
				}
				case br::WidgetType::Display: {
					NovaDisplay* d = new NovaDisplay;
					d->brModel = brModel;
					d->module = module;
					d->drawFn = w.draw;
					// Top-left corner, not the centre.
					d->box.pos = centre;
					d->box.size = size;
					addChild(d);
					break;
				}
				default:
					break;
			}
		}
	}

	void appendContextMenu(ui::Menu* menu) override {
		NovaModule* m = dynamic_cast<NovaModule*>(module);
		if (!m)
			return;
		br::Module* v0 = m->voices[0].get();

		// Settings the hardware keeps behind button combinations: the wrappers
		// expose them as params without a knob.
		bool separated = false;
		for (size_t i = 0; i < v0->params.size(); ++i) {
			const br::Param& p = v0->params[i];
			if (!p.hidden)
				continue;
			if (!separated) {
				menu->addChild(new ui::MenuSeparator);
				separated = true;
			}
			engine::ParamQuantity* pq = m->paramQuantities[i];
			if (!p.labels.empty()) {
				menu->addChild(createIndexSubmenuItem(p.name, p.labels,
					[=]() { return (size_t) std::max(0L, std::lround(pq->getValue() - pq->getMinValue())); },
					[=](size_t index) { pq->setValue(pq->getMinValue() + (float) index); }));
				continue;
			}
			bool snap = p.snap;
			menu->addChild(createSubmenuItem(p.name, pq->getDisplayValueString() + pq->getUnit(), [=](ui::Menu* sub) {
				sub->addChild(new NovaMenuSlider(pq));
				const int steps = snap ? 8 : 10;
				const float lo = pq->getMinValue(), hi = pq->getMaxValue();
				for (int s = 0; s <= steps; ++s) {
					float value = lo + (hi - lo) * (float) s / (float) steps;
					if (snap)
						value = std::round(value);
					const float tolerance = (hi - lo) * 0.5f / (float) steps;
					sub->addChild(createCheckMenuItem(stepLabel(pq, value), "",
						[=]() { return std::fabs(pq->getValue() - value) < tolerance * 0.2f; },
						[=]() { pq->setValue(value); }));
				}
			}));
		}

		std::vector<br::MenuAction> actions;
		v0->appendMenu(actions);
		if (!actions.empty() && !separated)
			menu->addChild(new ui::MenuSeparator);
		for (size_t i = 0; i < actions.size(); ++i) {
			std::string value = actions[i].value ? actions[i].value() : "";
			menu->addChild(createMenuItem(actions[i].label, value, [=]() {
				// Every voice keeps its own copy of the setting.
				for (auto& v : m->voices) {
					std::vector<br::MenuAction> own;
					v->appendMenu(own);
					if (i < own.size() && own[i].activate)
						own[i].activate(0);
				}
			}));
		}
	}
};

struct NovaModel : plugin::Model {
	const br::Model* brModel = NULL;
	NovaModelOptions options;

	engine::Module* createModule() override {
		engine::Module* m = new NovaModule(brModel, options);
		m->model = this;
		return m;
	}

	app::ModuleWidget* createModuleWidget(engine::Module* m) override {
		NovaModule* nm = NULL;
		if (m) {
			assert(m->model == this);
			nm = dynamic_cast<NovaModule*>(m);
		}
		app::ModuleWidget* mw = new NovaWidget(nm, brModel);
		assert(mw->module == m);
		mw->setModel(this);
		return mw;
	}
};

}  // namespace

plugin::Model* createNovaModel(const NovaModelOptions& options) {
	const br::Model* brModel = br::findModel(options.brSlug);
	if (!brModel) {
		WARN("Nova Modular: no BrickRack model \"%s\"", options.brSlug.c_str());
		return NULL;
	}
	NovaModel* model = new NovaModel;
	model->brModel = brModel;
	model->options = options;
	model->slug = options.slug;
	return model;
}
