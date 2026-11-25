/* See play_claims.hpp. */
#include <atomic>
#include <bitset>

#include <jni.h>

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Module.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>
#include <system.hpp>

#include "play_claims.hpp"

namespace rackdroid {


using namespace rack;


static const int PAD_DRIVER = -10;
static const int CELLS = 16;
static const double LOOK_SECONDS = 0.25;

static std::bitset<PLAY_CCS> g_claimed;
static double g_lookedAt = -1e9;
/** playTouchWanted(), and 8 for the sensors of movement: for the UI thread. */
static std::atomic<int> g_wanted{0};


static bool isModel(engine::Module* m, const char* plugin, const char* model) {
	return m && m->model && m->model->plugin && m->model->plugin->slug == plugin && m->model->slug == model;
}


static bool listensToPad(json_t* data) {
	json_t* midi = json_object_get(data, "midi");
	return json_integer_value(json_object_get(midi, "driver")) == PAD_DRIVER && json_object_get(midi, "deviceName");
}


std::vector<int> padCellsOf(engine::Module* m) {
	std::vector<int> cells;
	if (!isModel(m, "Core", "MIDICCToCVInterface"))
		return cells;
	json_t* data = m->dataToJson();
	if (listensToPad(data)) {
		for (int i = 0; i < CELLS; i++)
			cells.push_back((int) json_integer_value(json_array_get(json_object_get(data, "ccs"), i)));
	}
	json_decref(data);
	return cells;
}


static void claim(int cc) {
	if (cc >= 0 && cc < PLAY_CCS)
		g_claimed.set(cc);
}


static bool connected(app::ModuleWidget* mw, int output) {
	engine::Module* m = mw->module;
	return output >= 0 && output < m->getNumOutputs() && m->getOutput(output).isConnected();
}


/** Nova Play writes into its data what each of its outputs is, so that the two
need not know each other any better: "outputs", the CCs in the order of the
jacks, and "slots", whose jacks come after those. */
static void look(app::ModuleWidget* mw) {
	engine::Module* m = mw->module;
	std::vector<int> cells = padCellsOf(m);
	for (size_t i = 0; i < cells.size(); i++) {
		if (connected(mw, (int) i))
			claim(cells[i]);
	}
	bool isPlay = isModel(m, "NovaModular", "Play");
	bool isMap = isModel(m, "Core", "MIDI-Map");
	if (!isPlay && !isMap)
		return;
	json_t* data = m->dataToJson();
	size_t i;
	json_t* j;
	if (isMap && listensToPad(data)) {
		json_array_foreach(json_object_get(data, "maps"), i, j) {
			if (json_integer_value(json_object_get(j, "moduleId")) >= 0)
				claim((int) json_integer_value(json_object_get(j, "cc")));
		}
	}
	if (isPlay) {
		json_t* outputs = json_object_get(data, "outputs");
		json_array_foreach(outputs, i, j) {
			if (connected(mw, (int) i))
				claim((int) json_integer_value(j));
		}
		size_t first = json_array_size(outputs);
		json_array_foreach(json_object_get(data, "slots"), i, j) {
			json_t* target = json_object_get(j, "target");
			bool used = connected(mw, (int) (first + i)) || json_is_object(target);
			json_t* source = json_object_get(j, "source");
			if (!used || !json_is_integer(source) || json_integer_value(source) < 0)
				continue;
			claim((int) json_integer_value(source));
			size_t k;
			json_t* with;
			json_array_foreach(json_object_get(j, "with"), k, with) {
				claim((int) json_integer_value(with));
			}
		}
	}
	json_decref(data);
}


void playClaimsStep(bool now) {
	double t = system::getTime();
	if (!now && t - g_lookedAt < LOOK_SECONDS)
		return;
	g_lookedAt = t;
	g_claimed.reset();
	if (!APP->scene || !APP->scene->rack)
		return;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (mw->module)
			look(mw);
	}
	g_wanted.store(playTouchWanted() | (g_claimed.test(26) || g_claimed.test(27) || g_claimed.test(28) ? 8 : 0));
}


bool playClaimed(int cc) {
	return cc >= 0 && cc < PLAY_CCS && g_claimed.test(cc);
}


std::string playClaimsText() {
	struct Name {
		int from;
		int to;
		const char* name;
	};
	static const Name NAMES[] = {
		{2, 3, "R stick"}, {0, 1, "L stick"}, {4, 4, "L2"}, {5, 5, "R2"}, {6, 6, "A"}, {7, 7, "B"}, {8, 8, "X"}, {9, 9, "Y"},
		{10, 10, "L1"}, {11, 11, "R1"}, {17, 20, "D-pad"}, {15, 15, "L3"}, {21, 22, "tilt"}, {26, 26, "twist"}, {27, 28, "shake"},
		{23, 25, "pad"}, {29, 31, "pad"}, {64, 79, "drums"}, {80, 83, "faders"},
	};
	std::string out;
	const char* last = "";
	for (const Name& n : NAMES) {
		bool any = false;
		for (int cc = n.from; cc <= n.to; cc++)
			any = any || g_claimed.test(cc);
		if (!any || !std::string(last).compare(n.name))
			continue;
		out += (out.empty() ? "" : ", ") + std::string(n.name);
		last = n.name;
	}
	return out;
}


int playTouchWanted() {
	int wanted = 0;
	for (int cc = 0; cc < PLAY_CCS; cc++) {
		if (!g_claimed.test(cc))
			continue;
		if ((cc >= 23 && cc <= 25) || (cc >= 29 && cc <= 31))
			wanted |= 1;
		else if (cc >= 64 && cc < 80)
			wanted |= 2;
		else if (cc >= 80)
			wanted |= 4;
	}
	return wanted;
}


} // namespace rackdroid


extern "C" JNIEXPORT jint JNICALL
Java_org_rackdroid_MainActivity_nativePlayWanted(JNIEnv*, jobject) {
	return rackdroid::g_wanted.load();
}
