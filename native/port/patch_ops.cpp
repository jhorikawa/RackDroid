/* The patch, read and changed by request. See patch_ops.hpp.
 *
 *   turn                                        what follows is a new step of Undo
 *   patch                                       modules and cables
 *   module   {module}                           one module: knobs, jacks, cables
 *   model    {model}                            the same for one not in the patch
 *   add      {model, right_of?}
 *   remove   {module}
 *   connect  {from, output, to, input, stack?}
 *   disconnect {from, output, to, input}
 *   set      {module, param, value | text}
 *   listen   {module?}                          what the outputs do for a moment
 *   undo                                        the last turn, if nothing came after it
 *
 * Modules are called by small numbers here, given out as they are first
 * mentioned: Rack's own ids are sixteen digits, which is a lot to repeat.
 * Jacks and knobs go by name or by number.
 *
 * A request is carried out on the render thread, between frames, as everything
 * that touches the rack is. The caller waits; if nobody is drawing (the app is
 * not on the screen) it is told so, and the request is dropped rather than
 * carried out when the user comes back to a patch they may have changed.
 */
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <app/CableWidget.hpp>
#include <app/ModuleWidget.hpp>
#include <app/PortWidget.hpp>
#include <app/RackWidget.hpp>
#include <app/Scene.hpp>
#include <context.hpp>
#include <engine/Engine.hpp>
#include <engine/ParamQuantity.hpp>
#include <engine/PortInfo.hpp>
#include <history.hpp>
#include <plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>

#include <jansson.h>

#include "gamepad_nav.hpp"
#include "label_overlay.hpp"
#include "patch_ops.hpp"
#include "touch_input.hpp"
#include "transport.hpp"
#include "window_android.hpp"

#include <android/log.h>
#include <jni.h>
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.ops", __VA_ARGS__)

using namespace rack;

namespace rackdroid {


/** What went wrong, in words for whoever asked. */
struct OpError {
	std::string text;
};


/* ---- Names ---- */

static std::map<int64_t, int> g_numberOf;
static std::map<int, int64_t> g_idOf;
static int g_nextNumber = 1;


static int numberOf(int64_t id) {
	auto it = g_numberOf.find(id);
	if (it != g_numberOf.end())
		return it->second;
	int n = g_nextNumber++;
	g_numberOf[id] = n;
	g_idOf[n] = id;
	return n;
}


static app::RackWidget* theRack() {
	app::Scene* scene = APP->scene;
	if (!scene || !scene->rack)
		throw OpError{"The rack is not there yet."};
	return scene->rack;
}


static std::string nameOf(app::ModuleWidget* mw) {
	return string::f("%d %s", numberOf(mw->module->id), mw->model->name.c_str());
}


static app::ModuleWidget* moduleArg(json_t* req, const char* key) {
	json_t* j = json_object_get(req, key);
	int n = -1;
	if (json_is_integer(j))
		n = (int) json_integer_value(j);
	else if (json_is_string(j))
		n = std::atoi(json_string_value(j));
	auto it = g_idOf.find(n);
	app::ModuleWidget* mw = it != g_idOf.end() ? theRack()->getModule(it->second) : NULL;
	if (!mw)
		throw OpError{string::f("There is no module %d in the patch. Read the patch for the numbers.", n)};
	return mw;
}


static std::string foldName(const std::string& s) {
	return string::trim(string::lowercase(s));
}


/** An index into `names`, given as a number or as (the unmistakable part of) a
name. `what` is for the complaint. */
static int pick(json_t* j, const std::vector<std::string>& names, const std::string& what) {
	std::string asked;
	if (json_is_integer(j)) {
		int i = (int) json_integer_value(j);
		if (i >= 0 && i < (int) names.size())
			return i;
		asked = string::f("%d", i);
	}
	else if (json_is_string(j)) {
		asked = json_string_value(j);
		std::string f = foldName(asked);
		for (size_t i = 0; i < names.size(); i++) {
			if (foldName(names[i]) == f)
				return (int) i;
		}
		// "#3", or a bare "3" that is nobody's name.
		std::string digits = !f.empty() && f[0] == '#' ? f.substr(1) : f;
		if (!digits.empty() && digits.find_first_not_of("0123456789") == std::string::npos) {
			int i = std::atoi(digits.c_str());
			if (i < (int) names.size())
				return i;
		}
		int found = -1;
		for (size_t i = 0; !f.empty() && i < names.size(); i++) {
			if (foldName(names[i]).find(f) == std::string::npos)
				continue;
			if (found >= 0) {
				found = -2;
				break;
			}
			found = (int) i;
		}
		if (found >= 0)
			return found;
	}
	std::string list;
	for (size_t i = 0; i < names.size() && i < 48; i++)
		list += string::f("%s%d \"%s\"", i ? ", " : "", (int) i, names[i].c_str());
	throw OpError{string::f("%s has no \"%s\" (or more than one). It has: %s.", what.c_str(), asked.c_str(),
		list.empty() ? "none" : list.c_str())};
}


static std::vector<std::string> portNames(engine::Module* m, engine::Port::Type type) {
	std::vector<std::string> out;
	int n = type == engine::Port::INPUT ? m->getNumInputs() : m->getNumOutputs();
	for (int i = 0; i < n; i++) {
		engine::PortInfo* info = type == engine::Port::INPUT ? m->getInputInfo(i) : m->getOutputInfo(i);
		out.push_back(info ? info->getName() : string::f("#%d", i + 1));
	}
	return out;
}


static std::vector<std::string> paramNames(engine::Module* m) {
	std::vector<std::string> out;
	for (int i = 0; i < m->getNumParams(); i++) {
		engine::ParamQuantity* pq = m->getParamQuantity(i);
		out.push_back(pq ? pq->getLabel() : string::f("#%d", i + 1));
	}
	return out;
}


/* ---- Reading ---- */

static std::string endOf(app::PortWidget* port) {
	app::ModuleWidget* mw = port->getAncestorOfType<app::ModuleWidget>();
	std::vector<std::string> names = portNames(port->module, port->type);
	std::string name = port->portId < (int) names.size() ? names[port->portId] : "?";
	return string::f("%s \"%s\"", mw ? nameOf(mw).c_str() : "?", name.c_str());
}


static std::string cableText(app::CableWidget* cw) {
	return endOf(cw->outputPort) + " -> " + endOf(cw->inputPort);
}


static std::string number(float v) {
	return string::f("%.4g", v);
}


static std::string paramText(engine::ParamQuantity* pq) {
	std::string out = string::f("%d \"%s\": ", pq->paramId, pq->getLabel().c_str());
	if (pq->module->id >= 0)
		out += pq->getDisplayValueString() + pq->getUnit() + " ";
	engine::SwitchQuantity* sq = dynamic_cast<engine::SwitchQuantity*>(pq);
	if (sq && !sq->labels.empty() && sq->labels.size() <= 24) {
		out += "[choices: ";
		for (size_t i = 0; i < sq->labels.size(); i++)
			out += (i ? " | " : "") + sq->labels[i];
		return out + "]";
	}
	out += "[value ";
	if (pq->module->id >= 0)
		out += number(pq->getValue()) + " in ";
	out += number(pq->getMinValue()) + ".." + number(pq->getMaxValue());
	out += ", default " + number(pq->getDefaultValue());
	return out + (pq->snapEnabled ? ", whole numbers]" : "]");
}


static std::string describe(engine::Module* m, app::ModuleWidget* mw) {
	const int MAX_LISTED = 96;
	std::string out;
	if (m->getNumParams() > 0)
		out += "Knobs and switches:\n";
	int listed = 0;
	for (int i = 0; i < m->getNumParams(); i++) {
		engine::ParamQuantity* pq = m->getParamQuantity(i);
		// One that is on no panel is not the user's to see.
		if (!pq || (mw ? !mw->getParam(i) : pq->name.empty()))
			continue;
		if (++listed > MAX_LISTED) {
			out += string::f("  ... and more, %d in all\n", m->getNumParams());
			break;
		}
		out += "  " + paramText(pq) + "\n";
	}
	for (int t = 0; t < 2; t++) {
		engine::Port::Type type = t == 0 ? engine::Port::INPUT : engine::Port::OUTPUT;
		std::vector<std::string> names = portNames(m, type);
		std::string lines;
		for (size_t i = 0; i < names.size(); i++) {
			app::PortWidget* port = mw ? (t == 0 ? mw->getInput((int) i) : mw->getOutput((int) i)) : NULL;
			if (mw && !port)
				continue;
			engine::PortInfo* info = t == 0 ? m->getInputInfo((int) i) : m->getOutputInfo((int) i);
			lines += string::f("  %d \"%s\"", (int) i, names[i].c_str());
			std::string about = info ? info->getDescription() : "";
			if (!about.empty())
				lines += " (" + string::trim(about) + ")";
			if (port) {
				bool first = true;
				for (app::CableWidget* cw : theRack()->getCompleteCablesOnPort(port)) {
					lines += first ? (t == 0 ? " <- " : " -> ") : ", ";
					lines += endOf(t == 0 ? cw->outputPort : cw->inputPort);
					first = false;
				}
			}
			lines += "\n";
		}
		if (!lines.empty())
			out += std::string(t == 0 ? "Inputs:\n" : "Outputs:\n") + lines;
	}
	return out.empty() ? "It has no knobs and no jacks.\n" : out;
}


static std::string modelKey(plugin::Model* model) {
	return model->plugin->slug + "/" + model->slug;
}


static std::string opPatch() {
	app::RackWidget* rack = theRack();
	std::vector<app::ModuleWidget*> mods = rack->getModules();
	// As one reads a rack: row by row, left to right.
	std::sort(mods.begin(), mods.end(), [](app::ModuleWidget* a, app::ModuleWidget* b) {
		if (a->box.pos.y != b->box.pos.y)
			return a->box.pos.y < b->box.pos.y;
		return a->box.pos.x < b->box.pos.x;
	});
	std::string out;
	float y = NAN;
	int row = 0;
	for (app::ModuleWidget* mw : mods) {
		if (!mw->module)
			continue;
		if (mw->box.pos.y != y) {
			y = mw->box.pos.y;
			out += string::f("Row %d:\n", ++row);
		}
		out += string::f("  %s (%s)%s\n", nameOf(mw).c_str(), modelKey(mw->model).c_str(),
			mw->module->isBypassed() ? " bypassed" : "");
	}
	if (out.empty())
		out = "There are no modules in the patch.\n";
	std::string cables;
	for (app::CableWidget* cw : rack->getCompleteCables())
		cables += "  " + cableText(cw) + "\n";
	out += cables.empty() ? "No cables.\n" : "Cables:\n" + cables;
	CableFocus focus;
	if (gamepadNavCableFocus(focus)) {
		if (app::ModuleWidget* mw = rack->getModule(focus.moduleId))
			out += "The user has selected module " + nameOf(mw) + ".\n";
	}
	if (transportPaused())
		out += "The engine is paused.\n";
	return out;
}


static std::string opModule(json_t* req) {
	app::ModuleWidget* mw = moduleArg(req, "module");
	return string::f("%s (%s)\n", nameOf(mw).c_str(), modelKey(mw->model).c_str()) + describe(mw->module, mw);
}


static plugin::Model* modelArg(json_t* req) {
	const char* key = json_string_value(json_object_get(req, "model"));
	std::string k = key ? key : "";
	size_t slash = k.find('/');
	if (slash != std::string::npos) {
		for (plugin::Plugin* p : plugin::plugins) {
			if (p->slug != k.substr(0, slash))
				continue;
			for (plugin::Model* m : p->models) {
				if (m->slug == k.substr(slash + 1))
					return m;
			}
		}
	}
	throw OpError{"No module \"" + k + "\" is installed. A model is written Plugin/Module, as find_modules gives it."};
}


static std::string opModel(json_t* req) {
	plugin::Model* model = modelArg(req);
	std::unique_ptr<engine::Module> m(model->createModule());
	return modelKey(model) + " \"" + model->name + "\" by " + model->plugin->brand + "\n"
		+ (model->description.empty() ? "" : model->description + "\n") + describe(m.get(), NULL);
}


/* ---- Changing ---- */

/** One turn's changes. */
struct ChatAction : history::ComplexAction {
	int turn = 0;
};

static int g_turn = 0;
/** The module added last in this turn: the next goes beside it. */
static int64_t g_lastAdded = -1;


/** Where this turn's changes go: on the end of the history, as one action that
grows. A new one when something else came after it, or it was undone, or the
patch was saved with it half grown. */
static history::ComplexAction* group() {
	history::State* h = APP->history;
	if (!h->actions.empty() && h->actionIndex == (int) h->actions.size() && !h->isSaved()) {
		ChatAction* c = dynamic_cast<ChatAction*>(h->actions.back());
		if (c && c->turn == g_turn)
			return c;
	}
	ChatAction* c = new ChatAction;
	c->turn = g_turn;
	c->name = "chat";
	h->push(c);
	return c;
}


static void checkLock(int level) {
	if (touchLockMode() >= level)
		throw OpError{level == 1 ? "The user has locked the layout with the padlock in the toolbar: modules and cables cannot be changed."
			: "The user has locked everything with the padlock in the toolbar."};
}


/** Beside `anchor`, or on the end of the row being built, or on a new row when
that has grown too long to be looked at on a small screen. */
static void place(app::ModuleWidget* mw, app::ModuleWidget* anchor) {
	const float MAX_ROW = 100 * app::RACK_GRID_WIDTH;
	app::RackWidget* rack = theRack();
	std::vector<app::ModuleWidget*> mods = rack->getModules();
	if (anchor) {
		rack->setModulePosNearest(mw, math::Vec(anchor->box.getRight(), anchor->box.pos.y));
		return;
	}
	if (mods.empty()) {
		rack->setModulePosNearest(mw, app::RACK_OFFSET);
		return;
	}
	float left = INFINITY;
	float bottom = -INFINITY;
	for (app::ModuleWidget* o : mods) {
		left = std::fmin(left, o->box.pos.x);
		bottom = std::fmax(bottom, o->box.pos.y);
	}
	app::ModuleWidget* last = g_lastAdded >= 0 ? rack->getModule(g_lastAdded) : NULL;
	float y = last ? last->box.pos.y : bottom;
	float right = left;
	for (app::ModuleWidget* o : mods) {
		if (o->box.pos.y == y)
			right = std::fmax(right, o->box.getRight());
	}
	if (right + mw->box.size.x - left > MAX_ROW)
		rack->setModulePosNearest(mw, math::Vec(left, bottom + app::RACK_GRID_HEIGHT));
	else
		rack->setModulePosNearest(mw, math::Vec(right, y));
}


static std::string opAdd(json_t* req) {
	checkLock(1);
	plugin::Model* model = modelArg(req);
	app::ModuleWidget* anchor = json_object_get(req, "right_of") ? moduleArg(req, "right_of") : NULL;
	settings::ModuleInfo& mi = settings::moduleInfos[model->plugin->slug][model->slug];
	mi.added++;
	mi.lastAdded = system::getUnixTime();

	engine::Module* module = model->createModule();
	APP->engine->addModule(module);
	app::ModuleWidget* mw = model->createModuleWidget(module);
	place(mw, anchor);
	theRack()->addModule(mw);
	mw->loadTemplate();
	noteModuleAdded(module->id);
	g_lastAdded = module->id;

	history::ModuleAdd* h = new history::ModuleAdd;
	h->setModule(mw);
	group()->push(h);
	return "Added " + nameOf(mw) + " (" + modelKey(model) + ")\n" + describe(module, mw);
}


static std::string opRemove(json_t* req) {
	checkLock(1);
	app::ModuleWidget* mw = moduleArg(req, "module");
	std::string name = nameOf(mw);
	// As ModuleWidget::removeAction(), into this turn's action.
	history::ComplexAction* h = group();
	mw->appendDisconnectActions(h);
	theRack()->updateModuleOldPositions();
	if (settings::squeezeModules)
		theRack()->unsqueezeModulePos(mw);
	h->push(theRack()->getModuleDragAction());
	history::ModuleRemove* removal = new history::ModuleRemove;
	removal->setModule(mw);
	h->push(removal);
	theRack()->removeModule(mw);
	delete mw;
	theRack()->updateExpanders();
	return "Removed " + name + ".";
}


static app::PortWidget* portArg(json_t* req, const char* moduleKey, const char* portKey, engine::Port::Type type) {
	app::ModuleWidget* mw = moduleArg(req, moduleKey);
	bool in = type == engine::Port::INPUT;
	int i = pick(json_object_get(req, portKey), portNames(mw->module, type),
		"Module " + nameOf(mw) + (in ? ", among its inputs," : ", among its outputs,"));
	app::PortWidget* port = in ? mw->getInput(i) : mw->getOutput(i);
	if (!port)
		throw OpError{"That jack of " + nameOf(mw) + " is on no panel."};
	return port;
}


static void unplug(app::CableWidget* cw) {
	history::CableRemove* h = new history::CableRemove;
	h->setCable(cw);
	group()->push(h);
	theRack()->removeCable(cw);
	delete cw;
}


static std::string opConnect(json_t* req) {
	checkLock(1);
	app::PortWidget* out = portArg(req, "from", "output", engine::Port::OUTPUT);
	app::PortWidget* in = portArg(req, "to", "input", engine::Port::INPUT);
	app::RackWidget* rack = theRack();
	if (rack->getCable(out, in))
		return "Already connected: " + endOf(out) + " -> " + endOf(in);
	std::string replaced;
	if (!json_is_true(json_object_get(req, "stack"))) {
		for (app::CableWidget* cw : rack->getCompleteCablesOnPort(in)) {
			replaced += "\nUnplugged " + cableText(cw);
			unplug(cw);
		}
	}
	app::CableWidget* cw = new app::CableWidget;
	cw->color = rack->getNextCableColor();
	cw->inputPort = in;
	cw->outputPort = out;
	cw->updateCable();
	if (!cw->isComplete()) {
		delete cw;
		throw OpError{"Rack would not take that cable."};
	}
	rack->addCable(cw);
	history::CableAdd* h = new history::CableAdd;
	h->setCable(cw);
	group()->push(h);
	return "Connected " + cableText(cw) + replaced;
}


static std::string opDisconnect(json_t* req) {
	checkLock(1);
	app::PortWidget* out = portArg(req, "from", "output", engine::Port::OUTPUT);
	app::PortWidget* in = portArg(req, "to", "input", engine::Port::INPUT);
	app::CableWidget* cw = theRack()->getCable(out, in);
	if (!cw)
		throw OpError{"There is no cable " + endOf(out) + " -> " + endOf(in) + "."};
	std::string text = cableText(cw);
	unplug(cw);
	return "Unplugged " + text;
}


static std::string opSet(json_t* req) {
	checkLock(2);
	app::ModuleWidget* mw = moduleArg(req, "module");
	engine::Module* m = mw->module;
	int i = pick(json_object_get(req, "param"), paramNames(m), "Module " + nameOf(mw) + ", among its knobs,");
	engine::ParamQuantity* pq = m->getParamQuantity(i);
	if (!pq)
		throw OpError{"That knob cannot be set."};
	float old = pq->getValue();
	json_t* text = json_object_get(req, "text");
	json_t* value = json_object_get(req, "value");
	if (json_is_string(text))
		pq->setDisplayValueString(json_string_value(text));
	else if (json_is_number(value))
		pq->setValue((float) json_number_value(value));
	else
		throw OpError{"Give a value (a number) or a text."};
	float now = pq->getValue();
	if (now != old) {
		history::ParamChange* h = new history::ParamChange;
		h->moduleId = m->id;
		h->paramId = i;
		h->oldValue = old;
		h->newValue = now;
		group()->push(h);
	}
	return nameOf(mw) + " " + paramText(pq);
}


static std::string opUndo() {
	history::State* h = APP->history;
	ChatAction* c = h->canUndo() ? dynamic_cast<ChatAction*>(h->actions[h->actionIndex - 1]) : NULL;
	if (!c)
		throw OpError{"The last change to the patch was not made here: it is the user's to undo."};
	h->undo();
	g_turn++;
	return "Undone.";
}


static std::string opTurn() {
	g_turn++;
	g_lastAdded = -1;
	// Another patch altogether: start counting again.
	app::RackWidget* rack = theRack();
	bool known = false;
	for (auto& p : g_numberOf)
		known = known || rack->getModule(p.first);
	if (!known) {
		g_numberOf.clear();
		g_idOf.clear();
		g_nextNumber = 1;
	}
	return "";
}


/* ---- Listening ---- */

struct Request {
	std::string in;
	std::string out;
	bool done = false;
	/** Its caller has gone: not to be carried out. */
	bool dropped = false;
};

/** An output watched for a moment. Once a frame is no way to look at a wave,
but a second of it finds how far a wave swings, and that is the question. */
struct Watch {
	int64_t moduleId;
	engine::Port::Type type;
	int portId;
	float lo = INFINITY;
	float hi = -INFINITY;
	int channels = 0;
};

static std::shared_ptr<Request> g_listening;
static std::vector<Watch> g_watched;
static double g_listenUntil = 0.0;
static const double LISTEN_SECONDS = 1.2;


static bool isAudioOut(plugin::Model* model) {
	return model->plugin->slug == "Core" && string::startsWith(model->slug, "AudioInterface");
}


static void listenBegin(std::shared_ptr<Request> r, json_t* req) {
	if (transportPaused())
		throw OpError{"The engine is paused, so nothing moves. The user plays it again with SELECT."};
	app::ModuleWidget* only = json_object_get(req, "module") ? moduleArg(req, "module") : NULL;
	g_watched.clear();
	for (app::ModuleWidget* mw : theRack()->getModules()) {
		if (!mw->module || (only && only != mw))
			continue;
		for (app::PortWidget* p : mw->getPorts()) {
			bool wanted = p->type == engine::Port::OUTPUT ? (only || p->getPort()->isConnected())
				: isAudioOut(mw->model) && p->getPort()->isConnected();
			if (!wanted)
				continue;
			Watch w;
			w.moduleId = mw->module->id;
			w.type = p->type;
			w.portId = p->portId;
			g_watched.push_back(w);
		}
	}
	g_listening = r;
	g_listenUntil = system::getTime() + LISTEN_SECONDS;
}


static std::string listenEnd() {
	std::string out;
	for (Watch& w : g_watched) {
		app::ModuleWidget* mw = theRack()->getModule(w.moduleId);
		if (!mw || w.lo > w.hi)
			continue;
		bool in = w.type == engine::Port::INPUT;
		std::vector<std::string> names = portNames(mw->module, w.type);
		out += string::f("  %s \"%s\"%s: ", nameOf(mw).c_str(), names[w.portId].c_str(),
			in ? " (going to the speaker)" : "");
		if (w.hi - w.lo < 0.01f)
			out += string::f("steady at %.2f V", w.hi);
		else
			out += string::f("%.2f to %.2f V", w.lo, w.hi);
		if (w.channels > 1)
			out += string::f(", %d channels (the first measured)", w.channels);
		// Many modules leave an output alone until something is plugged in.
		if (w.channels == 0)
			out += " (nothing plugged in, so it may not be running)";
		out += "\n";
	}
	g_watched.clear();
	return out.empty() ? "Nothing is connected, so there is nothing to measure." : string::f("Over %.1f s:\n", LISTEN_SECONDS) + out;
}


static void listenSample() {
	for (Watch& w : g_watched) {
		engine::Module* m = APP->engine->getModule(w.moduleId);
		if (!m)
			continue;
		engine::Port* p = w.type == engine::Port::INPUT ? (engine::Port*) &m->getInput(w.portId) : (engine::Port*) &m->getOutput(w.portId);
		float v = p->getVoltage(0);
		w.channels = p->getChannels();
		if (!std::isfinite(v))
			continue;
		w.lo = std::fmin(w.lo, v);
		w.hi = std::fmax(w.hi, v);
	}
}


/* ---- The queue ---- */

static std::mutex g_mutex;
static std::condition_variable g_answered;
static std::deque<std::shared_ptr<Request>> g_queue;


static std::string answer(bool ok, const std::string& text) {
	json_t* j = json_object();
	json_object_set_new(j, "ok", json_boolean(ok));
	json_object_set_new(j, ok ? "text" : "error", json_stringn_nocheck(text.data(), text.size()));
	char* dump = json_dumps(j, JSON_COMPACT);
	std::string out = dump ? dump : "{\"ok\":false,\"error\":\"unreadable\"}";
	free(dump);
	json_decref(j);
	return out;
}


static void finish(const std::shared_ptr<Request>& r, const std::string& out) {
	std::lock_guard<std::mutex> lock(g_mutex);
	r->out = out;
	r->done = true;
	g_answered.notify_all();
}


/** Returns false if the answer comes later. */
static bool carryOut(const std::shared_ptr<Request>& r, std::string& out) {
	json_error_t error;
	json_t* req = json_loads(r->in.c_str(), 0, &error);
	if (!json_is_object(req)) {
		json_decref(req);
		out = answer(false, "not a JSON object");
		return true;
	}
	bool now = true;
	const char* opJ = json_string_value(json_object_get(req, "op"));
	std::string op = opJ ? opJ : "";
	try {
		std::string text;
		if (op == "turn") text = opTurn();
		else if (op == "patch") text = opPatch();
		else if (op == "module") text = opModule(req);
		else if (op == "model") text = opModel(req);
		else if (op == "add") text = opAdd(req);
		else if (op == "remove") text = opRemove(req);
		else if (op == "connect") text = opConnect(req);
		else if (op == "disconnect") text = opDisconnect(req);
		else if (op == "set") text = opSet(req);
		else if (op == "undo") text = opUndo();
		else if (op == "listen") {
			listenBegin(r, req);
			now = false;
		}
		else throw OpError{"There is no operation \"" + op + "\"."};
		out = answer(true, text);
	}
	catch (OpError& e) {
		out = answer(false, e.text);
	}
	catch (std::exception& e) {
		out = answer(false, e.what());
	}
	LOGI("%s: %s", op.c_str(), now ? (out.compare(0, 11, "{\"ok\":true,") == 0 ? "done" : out.c_str()) : "listening");
	json_decref(req);
	return now;
}


void patchOpsStep() {
	if (g_listening) {
		windowNoteInteraction();
		listenSample();
		if (system::getTime() >= g_listenUntil) {
			finish(g_listening, answer(true, listenEnd()));
			g_listening.reset();
		}
	}
	// A few to a frame: adding modules is slow, and the user is watching.
	for (int i = 0; i < 4; i++) {
		std::shared_ptr<Request> r;
		{
			std::lock_guard<std::mutex> lock(g_mutex);
			while (!g_queue.empty() && g_queue.front()->dropped)
				g_queue.pop_front();
			if (g_queue.empty() || g_listening)
				return;
			r = g_queue.front();
			g_queue.pop_front();
		}
		windowNoteInteraction();
		std::string out;
		if (carryOut(r, out))
			finish(r, out);
	}
}


std::string patchOpCall(const std::string& request) {
	std::shared_ptr<Request> r = std::make_shared<Request>();
	r->in = request;
	std::unique_lock<std::mutex> lock(g_mutex);
	g_queue.push_back(r);
	if (!g_answered.wait_for(lock, std::chrono::seconds(8), [&] { return r->done; })) {
		r->dropped = true;
		return answer(false, "RackDroid is not on the screen, so the patch cannot be read or changed. Nothing was done.");
	}
	return r->out;
}


} // namespace rackdroid


extern "C" JNIEXPORT jbyteArray JNICALL
Java_org_rackdroid_PatchServer_nativePatchOp(JNIEnv* env, jobject, jbyteArray request) {
	// Bytes, not a jstring: JNI's UTF-8 is not the real one.
	jsize n = env->GetArrayLength(request);
	std::string in((size_t) n, '\0');
	env->GetByteArrayRegion(request, 0, n, (jbyte*) &in[0]);
	std::string out = rackdroid::patchOpCall(in);
	jbyteArray result = env->NewByteArray((jsize) out.size());
	if (result)
		env->SetByteArrayRegion(result, 0, (jsize) out.size(), (const jbyte*) out.data());
	return result;
}
