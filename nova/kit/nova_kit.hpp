// What a module made on the Nova from the chat is written against.
// GPL-3.0-or-later.
//
// Such a module is a class with a constructor and process(); the panel, the
// menu, the state kept in the patch and the handling of mistakes are here. The
// same source builds for desktop VCV Rack (nova/tools/made-to-mac.sh), where
// there is no guard but the one against exceptions.
//
// guide.md, next to this file, is what the chat's model is given to read.

#pragma once

#include <rack.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef __ANDROID__
// native/port/module_guard.cpp, in the library the module is linked against.
extern "C" int nova_guard_call(void (*fn)(void*), void* arg, const void* owner, int where);
extern "C" void nova_guard_report(const void* owner, int where, const char* text);
#endif


namespace nova {


using namespace rack;


/* ---- Mistakes ---- */

/** As GuardWhere in module_guard.hpp. */
enum Where { MAKE, PROCESS, EVENT, STATE, DRAW, WIDGET, REMOVE };


template <class F>
inline bool guarded(const engine::Module* owner, Where where, F&& f) {
#ifdef __ANDROID__
	return nova_guard_call([](void* p) { (*static_cast<typename std::remove_reference<F>::type*>(p))(); },
		const_cast<void*>(static_cast<const void*>(&f)), owner, where) == 0;
#else
	(void) where;
	try {
		f();
		return true;
	}
	catch (const std::exception& e) {
		WARN("%s: %s", owner && owner->model ? owner->model->slug.c_str() : "a made module", e.what());
	}
	catch (...) {
	}
	return false;
#endif
}


inline void reportFault(const engine::Module* owner, Where where, const char* text) {
#ifdef __ANDROID__
	nova_guard_report(owner, where, text);
#else
	(void) where;
	WARN("%s: %s", owner && owner->model ? owner->model->slug.c_str() : "a made module", text);
#endif
}


/* ---- The panel, as it is asked for ---- */

struct Item {
	enum Kind { KNOB, BIG, SMALL, TRIM, IN, OUT, LIGHT, GAP };
	Kind kind;
	int id;
	std::string label;
};

/** A knob, or whatever suits the param: a switch of two or three positions, a
button. */
inline Item knob(int param, std::string label = "") { return {Item::KNOB, param, label}; }
inline Item big(int param, std::string label = "") { return {Item::BIG, param, label}; }
inline Item small(int param, std::string label = "") { return {Item::SMALL, param, label}; }
/** The smallest, for attenuverters. */
inline Item trim(int param, std::string label = "") { return {Item::TRIM, param, label}; }
inline Item in(int input, std::string label = "") { return {Item::IN, input, label}; }
inline Item out(int output, std::string label = "") { return {Item::OUT, output, label}; }
inline Item light(int light, std::string label = "") { return {Item::LIGHT, light, label}; }
inline Item gap() { return {Item::GAP, -1, ""}; }


struct Panel {
	/** In HP. As much as the rows need, if 0. */
	int width = 0;
	NVGcolor color = nvgRGB(0x1c, 0x24, 0x30);
	NVGcolor accent = nvgRGB(0x4f, 0xc3, 0xf7);
	std::string title;

	struct Row {
		std::vector<Item> items;
		float display = 0.f;
	};
	std::vector<Row> rows;

	/** From the top down. */
	void row(std::vector<Item> items) { rows.push_back({std::move(items), 0.f}); }
	/** Room for draw(), that high. */
	void display(float height) { rows.push_back({{}, height}); }
};


/* ---- What is kept in the patch ---- */

struct Kept {
	std::string key;
	std::function<json_t*()> save;
	std::function<void(json_t*)> load;
	std::function<void()> reset;
};

inline json_t* toJ(float v) { return json_real(v); }
inline json_t* toJ(double v) { return json_real(v); }
inline json_t* toJ(int v) { return json_integer(v); }
inline json_t* toJ(bool v) { return json_boolean(v); }
inline json_t* toJ(const std::string& v) { return json_stringn(v.data(), v.size()); }
inline void fromJ(json_t* j, float& v) { if (json_is_number(j)) v = (float) json_number_value(j); }
inline void fromJ(json_t* j, double& v) { if (json_is_number(j)) v = json_number_value(j); }
inline void fromJ(json_t* j, int& v) { if (json_is_number(j)) v = (int) json_number_value(j); }
inline void fromJ(json_t* j, bool& v) { if (json_is_boolean(j)) v = json_boolean_value(j); }
inline void fromJ(json_t* j, std::string& v) { if (json_is_string(j)) v = json_string_value(j); }

template <class C>
inline json_t* listToJ(const C& c) {
	json_t* j = json_array();
	for (size_t i = 0; i < c.size(); i++)
		json_array_append_new(j, toJ(typename C::value_type(c[i])));
	return j;
}

/** Only what there is room for: the list keeps its length. */
template <class C>
inline void listFromJ(json_t* j, C& c) {
	for (size_t i = 0; i < c.size() && i < json_array_size(j); i++) {
		typename C::value_type v = c[i];
		fromJ(json_array_get(j, i), v);
		c[i] = v;
	}
}

template <class T, size_t N>
inline json_t* toJ(const std::array<T, N>& v) { return listToJ(v); }
template <class T, size_t N>
inline void fromJ(json_t* j, std::array<T, N>& v) { listFromJ(j, v); }
template <class T>
inline json_t* toJ(const std::vector<T>& v) { return listToJ(v); }
template <class T>
inline void fromJ(json_t* j, std::vector<T>& v) { listFromJ(j, v); }


/* ---- The menu ---- */

struct MenuEntry {
	std::string label;
	std::vector<std::string> choices;
	int* choice = nullptr;
	bool* flag = nullptr;
	float* value = nullptr;
	float min = 0.f;
	float max = 1.f;
	float standard = 0.f;
	std::string unit;
};


/* ---- Small tools that modules often need ---- */

/** A delay line that cannot be read or written out of its range. */
struct Delay {
	std::vector<float> line = std::vector<float>(1, 0.f);
	size_t at = 0;

	/** Room for so many samples. Not in process(). */
	void resize(float samples) {
		line.assign((size_t) math::clamp(samples, 1.f, 4e6f) + 4, 0.f);
		at = 0;
	}
	void clear() { std::fill(line.begin(), line.end(), 0.f); }
	void push(float v) {
		at = at + 1 < line.size() ? at + 1 : 0;
		line[at] = std::isfinite(v) ? v : 0.f;
	}
	/** What was pushed so many samples ago, between samples too. 0 is the last. */
	float read(float ago) const {
		float most = (float) line.size() - 3.f;
		ago = ago > 0.f ? std::fmin(ago, std::fmax(most, 0.f)) : 0.f;
		size_t whole = (size_t) ago;
		float part = ago - (float) whole;
		return math::crossfade(tap(whole), tap(whole + 1), part);
	}

private:
	float tap(size_t ago) const {
		ago %= line.size();
		return line[at >= ago ? at - ago : at + line.size() - ago];
	}
};


/** Goes to a value in about `seconds`. */
struct Smooth {
	float value = 0.f;
	float process(float target, float seconds, float sampleTime) {
		float k = seconds > 0.f ? 1.f - std::exp(-sampleTime / seconds) : 1.f;
		value += (target - value) * k;
		return value;
	}
};


/** A filter (state variable, 12 dB) that stays stable however it is turned. */
struct Filter {
	float low = 0.f;
	float band = 0.f;
	float high = 0.f;
	float s1 = 0.f;
	float s2 = 0.f;

	void reset() { low = band = high = s1 = s2 = 0.f; }
	/** resonance 0..1 */
	void process(float x, float hz, float resonance, float sampleTime) {
		float g = std::tan(float(M_PI) * math::clamp(hz * sampleTime, 1e-5f, 0.49f));
		float k = 2.f - 1.98f * math::clamp(resonance, 0.f, 1.f);
		float h = (x - (k + g) * s1 - s2) / (1.f + g * (g + k));
		float b = g * h + s1;
		float l = g * b + s2;
		s1 = g * h + b;
		s2 = g * b + l;
		if (!std::isfinite(s1) || !std::isfinite(s2))
			s1 = s2 = 0.f;
		high = h;
		band = b;
		low = l;
	}
};


/* ---- The module ---- */

float engineRate();

struct Module : engine::Module {
	Panel panel;
	/** Of the engine. Right in the constructor and from then on. */
	float sampleRate = 48000.f;

	std::vector<Kept> kept;
	std::vector<MenuEntry> menu;
	/** Set once it has made a mistake: it is not called again. */
	std::atomic<bool> stopped{false};

	Module() { sampleRate = engineRate(); }

	/* What a module may have besides process(). */

	/** Initialize was chosen. Params and what is kept are back at their start. */
	virtual void reset() {}
	/** sampleRate is another one. Not while process() runs. */
	virtual void rateChanged() {}
	/** The display: size is its width and height, 0, 0 its top left corner. */
	virtual void draw(NVGcontext* vg, math::Vec size) {}

	/* For the constructor, after config(). */

	/** Kept in the patch, and put back by Initialize. */
	template <class T>
	void keep(const char* key, T* v) {
		T start = *v;
		kept.push_back({key,
			[v] { return toJ(*v); },
			[v](json_t* j) { fromJ(j, *v); },
			[v, start] { *v = start; }});
	}
	/** A choice in the module's menu; *v is the number of the choice, from 0.
	Kept as well. */
	void menuChoice(const char* key, std::string label, std::vector<std::string> choices, int* v) {
		MenuEntry e;
		e.label = std::move(label);
		e.choices = std::move(choices);
		e.choice = v;
		menu.push_back(e);
		int last = (int) e.choices.size() - 1;
		keep(key, v);
		Kept& k = kept.back();
		k.load = [v, last](json_t* j) {
			fromJ(j, *v);
			*v = math::clamp(*v, 0, std::max(last, 0));
		};
	}
	void menuSwitch(const char* key, std::string label, bool* v) {
		MenuEntry e;
		e.label = std::move(label);
		e.flag = v;
		menu.push_back(e);
		keep(key, v);
	}
	void menuSlider(const char* key, std::string label, float* v, float min, float max, std::string unit = "") {
		MenuEntry e;
		e.label = std::move(label);
		e.value = v;
		e.min = min;
		e.max = max;
		e.standard = *v;
		e.unit = std::move(unit);
		menu.push_back(e);
		keep(key, v);
		Kept& k = kept.back();
		k.load = [v, min, max](json_t* j) {
			fromJ(j, *v);
			*v = math::clamp(*v, min, max);
		};
	}

	/* Rack's side of it. These are final: use the ones above. */

	void onReset(const ResetEvent& e) override final {
		engine::Module::onReset(e);
		if (stopped)
			return;
		if (!guarded(this, EVENT, [&] {
				for (Kept& k : kept)
					k.reset();
				reset();
			}))
			stop();
	}
	void onRandomize(const RandomizeEvent& e) override final {
		engine::Module::onRandomize(e);
	}
	void onSampleRateChange(const SampleRateChangeEvent& e) override final {
		sampleRate = e.sampleRate;
		if (!stopped && !guarded(this, EVENT, [&] { rateChanged(); }))
			stop();
	}
	json_t* dataToJson() override final {
		json_t* root = json_object();
		guarded(this, STATE, [&] {
			for (Kept& k : kept)
				json_object_set_new(root, k.key.c_str(), k.save());
		});
		return root;
	}
	void dataFromJson(json_t* root) override final {
		guarded(this, STATE, [&] {
			for (Kept& k : kept) {
				json_t* j = json_object_get(root, k.key.c_str());
				if (j)
					k.load(j);
			}
		});
	}

	void stop() {
		stopped = true;
		for (engine::Output& o : outputs) {
			for (float& v : o.voltages)
				v = 0.f;
		}
	}
};


/** What is put into the rack: the module with a guard around process(). */
template <class T>
struct Guarded : T {
	int wrong = 0;

	void process(const engine::Module::ProcessArgs& args) override {
		if (this->stopped.load(std::memory_order_relaxed))
			return;
		if (!guarded(this, PROCESS, [&] { T::process(args); })) {
			this->stop();
			return;
		}
#ifndef NOVA_TEST
		bool finite = true;
		for (engine::Output& o : this->outputs) {
			int n = std::min((int) o.channels, (int) engine::PORT_MAX_CHANNELS);
			for (int c = 0; c < n; c++) {
				if (!std::isfinite(o.voltages[c])) {
					o.voltages[c] = 0.f;
					finite = false;
				}
			}
		}
		// Once may be a slip; a tenth of a second of it is a state that has
		// gone bad and does not come back.
		wrong = finite ? 0 : wrong + 1;
		if (wrong > args.sampleRate * 0.1f) {
			reportFault(this, PROCESS, "an output that is not a number (NaN or infinite), and stays so");
			this->stop();
		}
#endif
	}
};


/* ---- What is in nova_kit_impl.hpp ---- */

/** What a module is, as JSON, for the test and for the app. */
std::string describe(Module* m);

/** `preview` stands in for a module where there is none: the list of modules. */
app::ModuleWidget* makeWidget(Module* m, Module* preview, const std::string& slug);


template <class T>
struct Model : plugin::Model {
	std::unique_ptr<T> preview;

	engine::Module* createModule() override {
		engine::Module* m = new Guarded<T>;
		m->model = this;
		return m;
	}
	app::ModuleWidget* createModuleWidget(engine::Module* m) override {
#ifdef NOVA_TEST
		return NULL;
#else
		if (!m && !preview)
			preview.reset(new T);
		app::ModuleWidget* mw = makeWidget(static_cast<Module*>(m), preview.get(), slug);
		mw->setModel(this);
		return mw;
#endif
	}
};


template <class T>
inline plugin::Model* model(const std::string& slug) {
	Model<T>* m = new Model<T>;
	m->slug = slug;
	return m;
}


} // namespace nova


// Compiled once and for all on the device (nova_build.py), along with every
// module elsewhere.
#ifndef NOVA_SPLIT
#include "nova_kit_impl.hpp"
#endif


#define NOVA_EXPORT extern "C" __attribute__((visibility("default")))

/** Ends the source of a module: CLASS is in namespace NAMESPACE. */
#ifdef NOVA_DESKTOP
// Many modules make one plugin there (made-to-mac.sh).
#define NOVA_MODULE(NAMESPACE, CLASS) \
	rack::plugin::Model* novaModel_##CLASS() { return nova::model<NAMESPACE::CLASS>(#CLASS); }
#else
#define NOVA_MODULE(NAMESPACE, CLASS) \
	NOVA_EXPORT void init(rack::plugin::Plugin* p) { p->addModel(nova::model<NAMESPACE::CLASS>(#CLASS)); } \
	NOVA_EXPORT const char* nova_describe(rack::engine::Module* m) { \
		static std::string text; \
		text = nova::describe(static_cast<nova::Module*>(m)); \
		return text.c_str(); \
	} \
	NOVA_EXPORT void nova_draw(rack::engine::Module* m, NVGcontext* vg, float w, float h) { \
		static_cast<nova::Module*>(m)->draw(vg, rack::math::Vec(w, h)); \
	} \
	NOVA_EXPORT int nova_stopped(rack::engine::Module* m) { return static_cast<nova::Module*>(m)->stopped; }
#endif
