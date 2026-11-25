// Nova Modular - GPL-3.0-or-later.
//
// Nova Play: what a handheld with a game controller is played with, as voltages
// with names on them. It listens to Rack's "Gamepad" MIDI driver (-10): on the
// Nova that is the built-in controller with its sensors and touch surfaces
// (native/port/gamepad_midi.cpp, which also lists the CCs), on a computer the
// first gamepad, which has the sticks, the triggers and the buttons of it.
//
// Eight slots make one's own controls out of them: a button with others held, a
// stick that stays where it is let go, a switch; and each can move a knob that has
// no jack, away from where it stands and back.
//
// On the Nova, play_claims.cpp reads "outputs" and "slots" out of the saved data to
// know what the patch plays with.

#include "plugin.hpp"

#include <array>
#include <cmath>
#include <string>
#include <vector>

namespace {

enum Kind { BIPOLAR, UNIPOLAR, GATE };

struct Control {
	const char* label;
	const char* name;
	int cc;
	Kind kind;
};

// In the order of the jacks: columns of eight.
const Control CONTROLS[] = {
	{"L X", "Left stick left/right", 0, BIPOLAR}, {"L Y", "Left stick down/up", 1, BIPOLAR},
	{"R X", "Right stick left/right", 2, BIPOLAR}, {"R Y", "Right stick down/up", 3, BIPOLAR},
	{"L2", "L2", 4, UNIPOLAR}, {"R2", "R2", 5, UNIPOLAR}, {"L1", "L1", 10, GATE}, {"R1", "R1", 11, GATE},
	{"A", "A", 6, GATE}, {"B", "B", 7, GATE}, {"X", "X", 8, GATE}, {"Y", "Y", 9, GATE},
	{"UP", "D-pad up", 17, GATE}, {"RIGHT", "D-pad right", 18, GATE}, {"DOWN", "D-pad down", 19, GATE}, {"LEFT", "D-pad left", 20, GATE},
	{"L3", "Left stick pressed in", 15, GATE}, {"TILT X", "Tilt left/right", 21, BIPOLAR}, {"TILT Y", "Tilt towards/away", 22, BIPOLAR},
	{"TWIST", "Twist", 26, BIPOLAR}, {"SHAKE", "Shake", 27, UNIPOLAR}, {"SHAKEN", "Shaken", 28, GATE},
	{"FADE 1", "Fader 1", 80, UNIPOLAR}, {"FADE 2", "Fader 2", 81, UNIPOLAR},
	{"PAD X", "Touch pad left/right", 23, UNIPOLAR}, {"PAD Y", "Touch pad down/up", 24, UNIPOLAR}, {"TOUCH", "Touch pad touched", 25, GATE},
	{"2 X", "Second finger left/right", 29, UNIPOLAR}, {"2 Y", "Second finger down/up", 30, UNIPOLAR}, {"2 TOUCH", "Second finger down", 31, GATE},
	{"FADE 3", "Fader 3", 82, UNIPOLAR}, {"FADE 4", "Fader 4", 83, UNIPOLAR},
	{"DRUM 1", "Drum pad 1", 64, GATE}, {"DRUM 2", "Drum pad 2", 65, GATE}, {"DRUM 3", "Drum pad 3", 66, GATE}, {"DRUM 4", "Drum pad 4", 67, GATE},
	{"DRUM 5", "Drum pad 5", 68, GATE}, {"DRUM 6", "Drum pad 6", 69, GATE}, {"DRUM 7", "Drum pad 7", 70, GATE}, {"DRUM 8", "Drum pad 8", 71, GATE},
	{"LEV 1", "Drum pad 1 level", 72, UNIPOLAR}, {"LEV 2", "Drum pad 2 level", 73, UNIPOLAR}, {"LEV 3", "Drum pad 3 level", 74, UNIPOLAR},
	{"LEV 4", "Drum pad 4 level", 75, UNIPOLAR}, {"LEV 5", "Drum pad 5 level", 76, UNIPOLAR}, {"LEV 6", "Drum pad 6 level", 77, UNIPOLAR},
	{"LEV 7", "Drum pad 7 level", 78, UNIPOLAR}, {"LEV 8", "Drum pad 8 level", 79, UNIPOLAR},
};
const int NUM_CONTROLS = sizeof(CONTROLS) / sizeof(CONTROLS[0]);
const int SLOTS = 8;
const int NUM_CCS = 128;
const int DRIVER = -10;
const int ROWS = 8;

const std::vector<std::string> MODES = {"Follow", "Stay", "Gate", "Switch", "Trigger"};
enum Mode { FOLLOW, STAY, GATED, SWITCHED, TRIGGERED };

const Control* controlOf(int cc) {
	for (const Control& c : CONTROLS) {
		if (c.cc == cc)
			return &c;
	}
	return NULL;
}

struct Slot {
	int source = -1;
	std::array<int, 2> with{{-1, -1}};
	int mode = FOLLOW;
	float lo = 0.f;
	float hi = 10.f;
	/** The knob it moves, and how far of its range at full. */
	ParamHandle handle;
	float amount = 0.f;
	/** Where the knob stands when it is left alone, and that it is away from there. */
	float base = 0.f;
	bool away = false;
	bool restore = false;

	float value = 0.f;
	bool wasOn = false;
	dsp::PulseGenerator pulse;
};


struct Play : Module {
	enum { NUM_PARAMS };
	enum { NUM_INPUTS };
	enum { SLOT_OUTPUT = NUM_CONTROLS, NUM_OUTPUTS = NUM_CONTROLS + SLOTS };

	midi::InputQueue midiInput;
	std::array<int8_t, NUM_CCS> msb{};
	std::array<int8_t, NUM_CCS> lsb{};
	std::array<float, NUM_CCS> smooth{};
	std::array<bool, NUM_CCS> taken{};
	std::array<Slot, SLOTS> slots;
	/** The slot that takes the next knob touched, or -1. */
	int learning = -1;

	Play() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS);
		for (int i = 0; i < NUM_CONTROLS; i++)
			configOutput(i, CONTROLS[i].name);
		for (int i = 0; i < SLOTS; i++) {
			configOutput(SLOT_OUTPUT + i, string::f("Slot %d", i + 1));
			slots[i].handle.color = nvgRGB(0x8d, 0xe0, 0x7a);
			slots[i].handle.text = string::f("Nova Play slot %d", i + 1);
			APP->engine->addParamHandle(&slots[i].handle);
		}
		attach();
	}

	~Play() {
		for (Slot& s : slots)
			APP->engine->removeParamHandle(&s.handle);
	}

	/** To the first gamepad there is. */
	void attach() {
		midiInput.setDriverId(DRIVER);
		if (midiInput.getDriverId() != DRIVER)
			return;
		std::vector<int> ids = midiInput.getDeviceIds();
		if (!ids.empty())
			midiInput.setDeviceId(ids[0]);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		for (Slot& s : slots) {
			APP->engine->updateParamHandle_NoLock(&s.handle, -1, 0, true);
			s.source = -1;
			s.with = {{-1, -1}};
			s.mode = FOLLOW;
			s.lo = 0.f;
			s.hi = 10.f;
			s.amount = 0.f;
			s.value = 0.f;
			s.away = false;
		}
		learning = -1;
	}

	void read(const midi::Message& m) {
		if (m.getStatus() != 0xb || m.bytes.size() < 3)
			return;
		int cc = m.getNote();
		int8_t v = (int8_t) m.bytes[2];
		// The fine half of 0..31 comes as 32..63.
		if (cc >= 32 && cc < 64)
			lsb[cc - 32] = v;
		else if (cc < NUM_CCS)
			msb[cc] = v;
	}

	float raw(int cc) const {
		return cc < 0 || cc >= NUM_CCS ? 0.f : smooth[cc];
	}

	bool on(int cc) const {
		return std::fabs(raw(cc)) > 0.5f;
	}

	bool condition(const Slot& s) const {
		for (int w : s.with) {
			if (w >= 0 && !on(w))
				return false;
		}
		return true;
	}

	static bool conditional(const Slot& s) {
		return s.with[0] >= 0 || s.with[1] >= 0;
	}

	void stepSlot(Slot& s, float sampleTime) {
		bool held = condition(s);
		float r = raw(s.source);
		bool mine = held && (conditional(s) || !taken[s.source]);
		bool isOn = mine && on(s.source);
		switch (s.mode) {
			case FOLLOW: s.value = mine ? r : 0.f; break;
			case STAY:
				// With a button held it is set, and let go it stays; without one
				// it is pushed up and down, as a stick comes back by itself.
				if (conditional(s)) {
					if (held)
						s.value = r;
				}
				else if (mine) {
					s.value = clamp(s.value + r * sampleTime, -1.f, 1.f);
				}
				break;
			case GATED: s.value = isOn ? 1.f : 0.f; break;
			case SWITCHED:
				if (isOn && !s.wasOn)
					s.value = s.value > 0.5f ? 0.f : 1.f;
				break;
			case TRIGGERED:
				if (isOn && !s.wasOn)
					s.pulse.trigger(0.01f);
				s.value = s.pulse.process(sampleTime) ? 1.f : 0.f;
				break;
		}
		s.wasOn = isOn;
	}

	float volts(const Slot& s) const {
		const Control* c = controlOf(s.source);
		bool bipolar = c && c->kind == BIPOLAR && (s.mode == FOLLOW || s.mode == STAY);
		float part = bipolar ? (s.value + 1.f) * 0.5f : s.value;
		return rescale(part, 0.f, 1.f, s.lo, s.hi);
	}

	/** The knob, from where it stands and back. */
	void stepKnob(Slot& s) {
		if (!s.handle.module)
			return;
		if (s.handle.paramId >= (int) s.handle.module->paramQuantities.size())
			return;
		ParamQuantity* pq = s.handle.module->paramQuantities[s.handle.paramId];
		if (!pq || !pq->isBounded())
			return;
		if (s.restore) {
			s.restore = false;
			if (s.away)
				pq->setValue(s.base);
			s.away = false;
		}
		if (s.source < 0)
			return;
		if (std::fabs(s.value) < 0.002f) {
			if (s.away)
				pq->setValue(s.base);
			s.away = false;
			// The user's, while nothing is played.
			s.base = pq->getValue();
			return;
		}
		s.away = true;
		float to = s.base + s.value * s.amount * (pq->getMaxValue() - pq->getMinValue());
		pq->setValue(clamp(to, pq->getMinValue(), pq->getMaxValue()));
	}

	void process(const ProcessArgs& args) override {
		midi::Message m;
		while (midiInput.tryPop(&m, args.frame))
			read(m);
		float k = 1.f - std::exp(-args.sampleTime / 0.008f);
		taken.fill(false);
		for (const Slot& s : slots) {
			if (s.source >= 0 && conditional(s) && condition(s))
				taken[s.source] = true;
		}
		for (int i = 0; i < NUM_CONTROLS; i++) {
			const Control& c = CONTROLS[i];
			float to = (msb[c.cc] * 128 + (c.cc < 32 && c.kind != GATE ? lsb[c.cc] : 0)) / (128.f * 127.f);
			to = clamp(to, -1.f, 1.f);
			// Gates, and what jumps, at once.
			if (c.kind == GATE)
				smooth[c.cc] = to;
			else
				smooth[c.cc] += (to - smooth[c.cc]) * k;
			if (!outputs[i].isConnected())
				continue;
			// Another has it while its buttons are held: a gate is off, the rest
			// stays where it was.
			if (!taken[c.cc])
				outputs[i].setVoltage(smooth[c.cc] * 10.f);
			else if (c.kind == GATE)
				outputs[i].setVoltage(0.f);
		}
		for (int i = 0; i < SLOTS; i++) {
			Slot& s = slots[i];
			if (s.source >= 0)
				stepSlot(s, args.sampleTime);
			outputs[SLOT_OUTPUT + i].setVoltage(s.source >= 0 ? volts(s) : 0.f);
			if ((args.frame + i) % 32 == 0)
				stepKnob(s);
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "midi", midiInput.toJson());
		json_t* outs = json_array();
		for (const Control& c : CONTROLS)
			json_array_append_new(outs, json_integer(c.cc));
		json_object_set_new(root, "outputs", outs);
		json_t* list = json_array();
		for (Slot& s : slots) {
			json_t* j = json_object();
			json_object_set_new(j, "source", json_integer(s.source));
			json_t* with = json_array();
			for (int w : s.with)
				json_array_append_new(with, json_integer(w));
			json_object_set_new(j, "with", with);
			json_object_set_new(j, "mode", json_integer(s.mode));
			json_object_set_new(j, "lo", json_real(s.lo));
			json_object_set_new(j, "hi", json_real(s.hi));
			if (s.handle.moduleId >= 0) {
				json_t* t = json_object();
				json_object_set_new(t, "moduleId", json_integer(s.handle.moduleId));
				json_object_set_new(t, "paramId", json_integer(s.handle.paramId));
				json_object_set_new(t, "amount", json_real(s.amount));
				json_object_set_new(t, "base", json_real(s.base));
				json_object_set_new(t, "away", json_boolean(s.away));
				json_object_set_new(j, "target", t);
			}
			json_array_append_new(list, j);
		}
		json_object_set_new(root, "slots", list);
		return root;
	}

	void dataFromJson(json_t* root) override {
		json_t* midiJ = json_object_get(root, "midi");
		if (midiJ)
			midiInput.fromJson(midiJ);
		// Saved on another machine, where the gamepad has another name.
		if (midiInput.getDeviceId() < 0)
			attach();
		for (int i = 0; i < SLOTS; i++) {
			json_t* j = json_array_get(json_object_get(root, "slots"), i);
			if (!json_is_object(j))
				continue;
			Slot& s = slots[i];
			s.source = (int) json_integer_value(json_object_get(j, "source"));
			if (!controlOf(s.source))
				s.source = -1;
			for (size_t w = 0; w < s.with.size(); w++) {
				json_t* wj = json_array_get(json_object_get(j, "with"), w);
				int cc = json_is_integer(wj) ? (int) json_integer_value(wj) : -1;
				s.with[w] = controlOf(cc) ? cc : -1;
			}
			s.mode = clamp((int) json_integer_value(json_object_get(j, "mode")), 0, (int) MODES.size() - 1);
			s.lo = (float) json_number_value(json_object_get(j, "lo"));
			s.hi = (float) json_number_value(json_object_get(j, "hi"));
			s.value = 0.f;
			s.wasOn = false;
			json_t* t = json_object_get(j, "target");
			int64_t moduleId = -1;
			int paramId = 0;
			s.away = false;
			if (json_is_object(t)) {
				moduleId = json_integer_value(json_object_get(t, "moduleId"));
				paramId = (int) json_integer_value(json_object_get(t, "paramId"));
				s.amount = (float) json_number_value(json_object_get(t, "amount"));
				s.base = (float) json_number_value(json_object_get(t, "base"));
				s.away = json_is_true(json_object_get(t, "away"));
				s.restore = s.away;
			}
			APP->engine->updateParamHandle_NoLock(&s.handle, moduleId, paramId, true);
		}
	}
};


int fontHandle() {
	std::shared_ptr<window::Font> font = APP->window->loadFont(asset::plugin(pluginInstance, "res/fonts/BarlowCondensed-SemiBold.ttf"));
	return font ? font->handle : -1;
}

const float COLUMN = 34.f;
const float LEFT = 18.f;
const float TOP = 52.f;
const float ROW = 40.f;

Vec jackAt(int i) {
	return Vec(LEFT + COLUMN * (i / ROWS), TOP + ROW * (i % ROWS) + 14.f);
}

struct PlayPanel : widget::Widget {
	Play* module = NULL;

	void draw(const DrawArgs& args) override {
		NVGcontext* vg = args.vg;
		nvgBeginPath(vg);
		nvgRect(vg, 0, 0, box.size.x, box.size.y);
		nvgFillPaint(vg, nvgLinearGradient(vg, 0, 0, 0, box.size.y, nvgRGB(0x2b, 0x33, 0x40), nvgRGB(0x1b, 0x20, 0x29)));
		nvgFill(vg);
		nvgBeginPath(vg);
		nvgRect(vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
		nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 40));
		nvgStrokeWidth(vg, 1.f);
		nvgStroke(vg);
		// The slots are one's own.
		float x = jackAt(NUM_CONTROLS).x - COLUMN * 0.5f;
		nvgBeginPath(vg);
		nvgRoundedRect(vg, x + 1.f, TOP - 16.f, COLUMN - 2.f, ROW * ROWS + 8.f, 4.f);
		nvgFillColor(vg, nvgRGBA(0x8d, 0xe0, 0x7a, 44));
		nvgFill(vg);

		int font = fontHandle();
		if (font < 0)
			return;
		nvgFontFaceId(vg, font);
		nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
		nvgFillColor(vg, nvgRGB(0xf4, 0xf6, 0xf8));
		nvgFontSize(vg, 20.f);
		nvgText(vg, box.size.x * 0.5f, 22.f, "NOVA PLAY", NULL);
		nvgFontSize(vg, 10.5f);
		for (int i = 0; i < NUM_CONTROLS + SLOTS; i++) {
			std::string text = i < NUM_CONTROLS ? CONTROLS[i].label : string::f("SLOT %d", i - NUM_CONTROLS + 1);
			Vec p = jackAt(i);
			nvgText(vg, p.x, p.y - 14.5f, text.c_str(), NULL);
		}
	}
};


struct PlayWidget : ModuleWidget {
	PlayWidget(Play* module) {
		setModule(module);
		box.size = Vec(RACK_GRID_WIDTH * 16, RACK_GRID_HEIGHT);
		PlayPanel* panel = new PlayPanel;
		panel->module = module;
		panel->box.size = box.size;
		widget::FramebufferWidget* fb = new widget::FramebufferWidget;
		fb->box.size = box.size;
		fb->addChild(panel);
		addChild(fb);
		for (int i = 0; i < NUM_CONTROLS + SLOTS; i++)
			addOutput(createOutputCentered<PJ301MPort>(jackAt(i), module, i));
	}

	void step() override {
		ModuleWidget::step();
		Play* play = dynamic_cast<Play*>(module);
		if (!play || play->learning < 0)
			return;
		ParamWidget* touched = APP->scene->rack->getTouchedParam();
		if (!touched || !touched->module || touched->module == module)
			return;
		Slot& s = play->slots[play->learning];
		APP->engine->updateParamHandle(&s.handle, touched->module->id, touched->paramId, true);
		if (s.amount == 0.f)
			s.amount = 0.5f;
		APP->scene->rack->setTouchedParam(NULL);
		play->learning = -1;
	}

	static void setSource(Slot& s, int cc) {
		s.source = cc;
		const Control* c = controlOf(cc);
		s.lo = c && c->kind == BIPOLAR ? -10.f : 0.f;
		s.hi = 10.f;
	}

	static void sources(ui::Menu* menu, std::function<int()> get, std::function<void(int)> set, bool buttonsOnly) {
		menu->addChild(createCheckMenuItem("None", "", [=]() { return get() < 0; }, [=]() { set(-1); }));
		for (const Control& c : CONTROLS) {
			int cc = c.cc;
			if (buttonsOnly && (c.kind != GATE || cc >= 64))
				continue;
			menu->addChild(createCheckMenuItem(c.name, "", [=]() { return get() == cc; }, [=]() { set(cc); }));
		}
	}

	void slotMenu(ui::Menu* menu, int i) {
		Play* play = dynamic_cast<Play*>(module);
		Slot* s = &play->slots[i];
		menu->addChild(createSubmenuItem("Control", controlOf(s->source) ? controlOf(s->source)->name : "None", [=](ui::Menu* m) {
			sources(m, [=]() { return s->source; }, [=](int cc) { setSource(*s, cc); }, false);
		}));
		for (int w = 0; w < 2; w++) {
			const Control* c = controlOf(s->with[w]);
			menu->addChild(createSubmenuItem(w ? "and with" : "With held", c ? c->name : "None", [=](ui::Menu* m) {
				sources(m, [=]() { return s->with[w]; }, [=](int cc) { s->with[w] = cc; }, true);
			}));
		}
		menu->addChild(createIndexSubmenuItem("Mode", MODES, [=]() { return s->mode; }, [=](int mode) { s->mode = mode; s->value = 0.f; }));
		menu->addChild(createSubmenuItem("Range", string::f("%g to %g V", s->lo, s->hi), [=](ui::Menu* m) {
			static const float RANGES[][2] = {{-10, 10}, {-5, 5}, {-1, 1}, {0, 10}, {0, 5}, {0, 1}, {10, 0}, {5, 0}};
			for (const auto& r : RANGES) {
				float lo = r[0], hi = r[1];
				m->addChild(createCheckMenuItem(string::f("%g to %g V", lo, hi), "", [=]() { return s->lo == lo && s->hi == hi; },
					[=]() { s->lo = lo; s->hi = hi; }));
			}
		}));
		std::string knob = "None";
		if (s->handle.module && s->handle.paramId < (int) s->handle.module->paramQuantities.size())
			knob = s->handle.module->paramQuantities[s->handle.paramId]->getLabel();
		menu->addChild(createSubmenuItem("Knob", play->learning == i ? "touch one" : knob, [=](ui::Menu* m) {
			m->addChild(createMenuItem("The next one touched", "", [=]() { play->learning = i; }));
			m->addChild(createMenuItem("None", "", [=]() {
				play->learning = -1;
				APP->engine->updateParamHandle(&s->handle, -1, 0, true);
			}));
		}));
		menu->addChild(createSubmenuItem("How far", string::f("%g%%", s->amount * 100.f), [=](ui::Menu* m) {
			for (float a : {1.f, 0.5f, 0.25f, 0.1f, -0.1f, -0.25f, -0.5f, -1.f})
				m->addChild(createCheckMenuItem(string::f("%g%%", a * 100.f), "", [=]() { return s->amount == a; }, [=]() { s->amount = a; }));
		}));
	}

	void appendContextMenu(ui::Menu* menu) override {
		Play* play = dynamic_cast<Play*>(module);
		if (!play)
			return;
		menu->addChild(new ui::MenuSeparator);
		for (int i = 0; i < SLOTS; i++) {
			const Control* c = controlOf(play->slots[i].source);
			menu->addChild(createSubmenuItem(string::f("Slot %d", i + 1), c ? c->name : "", [=](ui::Menu* m) { slotMenu(m, i); }));
		}
		menu->addChild(new ui::MenuSeparator);
		menu->addChild(createSubmenuItem("Controller", "", [=](ui::Menu* m) { appendMidiMenu(m, &play->midiInput); }));
	}
};

} // namespace


plugin::Model* createPlayModel() {
	return createModel<Play, PlayWidget>("Play");
}
