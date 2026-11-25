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
 *   menu     {module}                           the module's own menu, and its presets
 *   choose   {module, item, value? | text?}     a row of it
 *   act      {module, what}                     bypass, unbypass, initialize, randomize,
 *                                               duplicate, duplicate_with_cables, unplug_all
 *   move     {module, where, other}             right_of, left_of, below or above another
 *   controls {controls, right_of?}              play mode's controls as outputs
 *   listen   {module?}                          what the outputs do for a moment
 *   undo                                        the last turn, if nothing came after it
 *   play     {on}                               not a step of Undo, nor are these:
 *   patches                                     the saved patches, and which is open
 *   save     {name?, overwrite?}
 *   open     {name, unsaved?: keep | discard}   without a name, an empty one
 *
 * Those that begin with "made_" are about modules made on this device:
 * made_modules.cpp.
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
#include <cstring>
#include <ctime>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
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
#include <asset.hpp>
#include <patch.hpp>
#include <plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>
#include <ui/Menu.hpp>
#include <ui/MenuItem.hpp>
#include <ui/MenuLabel.hpp>
#include <ui/Slider.hpp>
#include <ui/TextField.hpp>

#include <jansson.h>

#include "gamepad_input.hpp"
#include "gamepad_nav.hpp"
#include "label_overlay.hpp"
#include "made_modules.hpp"
#include "patch_ops.hpp"
#include "play_claims.hpp"
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


static std::string foldName(std::string s) {
	std::replace(s.begin(), s.end(), '"', '\'');
	s = string::trim(string::lowercase(s));
	// A name given back with the marks it was listed in.
	while (s.size() >= 2 && s.front() == '\'' && s.back() == '\'' )
		s = s.substr(1, s.size() - 2);
	return s;
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
		std::string name = info ? info->getName() : string::f("#%d", i + 1);
		// Audio's jacks have quotation marks in their names.
		std::replace(name.begin(), name.end(), '"', '\'');
		out.push_back(name);
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
	return string::f("%s [%d \"%s\"]", mw ? nameOf(mw).c_str() : "?", port->portId, name.c_str());
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


/** The controls of play mode, as gamepad_midi.cpp sends them. */
struct Control {
	const char* name;
	int cc;
	const char* what;
};
static const Control CONTROLS[] = {
	{"stick_x", 2, "right stick left/right, -10..10 V in play mode"},
	{"stick_y", 3, "right stick down/up, -10..10 V in play mode"},
	{"l3", 15, "left stick pressed in, a gate in play mode"},
	{"tilt_x", 21, "tilt left/right, -10..10 V in play mode"},
	{"tilt_y", 22, "tilt towards/away, -10..10 V in play mode"},
	{"pad_x", 23, "touch pad left/right, 0..10 V"},
	{"pad_y", 24, "touch pad down/up, 0..10 V"},
	{"pad_gate", 25, "a finger on the touch pad, a gate"},
};
static const int PAD_DRIVER = -10;
static const char PAD_DEVICE[] = "Built-in controller";
static const int CELLS = 16;


static std::string describe(engine::Module* m, app::ModuleWidget* mw) {
	std::vector<int> cells = padCellsOf(m);
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
			if (t == 1 && i < cells.size()) {
				for (const Control& c : CONTROLS) {
					if (c.cc == cells[i])
						lines += std::string(" (") + c.name + ": " + c.what + ")";
				}
			}
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
		std::string stopped = madeStatus(mw->module);
		out += string::f("  %s (%s)%s%s\n", nameOf(mw).c_str(), modelKey(mw->model).c_str(),
			mw->module->isBypassed() ? " bypassed" : "", stopped.empty() ? "" : (" " + stopped).c_str());
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
	if (gamepadPlayMode())
		out += "Play mode is on.\n";
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
	gamepadNavShow(module->id);
	return "Added " + nameOf(mw) + " (" + modelKey(model) + ")\n" + describe(module, mw);
}


/** Play mode's controls on the outputs of a MIDI-CC to CV module: one that
listens to the controller already, or a new one. */
static std::string opControls(json_t* req) {
	checkLock(1);
	std::vector<const Control*> wanted;
	size_t i;
	json_t* j;
	json_array_foreach(json_object_get(req, "controls"), i, j) {
		const char* name = json_string_value(j);
		const Control* found = NULL;
		for (const Control& c : CONTROLS) {
			if (name && !std::strcmp(name, c.name))
				found = &c;
		}
		if (!found)
			throw OpError{std::string("There is no control \"") + (name ? name : "?") + "\"."};
		wanted.push_back(found);
	}
	if (wanted.empty())
		throw OpError{"Which controls?"};

	app::ModuleWidget* mw = NULL;
	for (app::ModuleWidget* o : theRack()->getModules()) {
		if (!padCellsOf(o->module).empty())
			mw = o;
	}
	bool isNew = !mw;
	if (isNew) {
		plugin::Model* model = plugin::getModel("Core", "MIDICCToCVInterface");
		if (!model)
			throw OpError{"Core's MIDI-CC to CV is not there."};
		app::ModuleWidget* anchor = json_object_get(req, "right_of") ? moduleArg(req, "right_of") : NULL;
		engine::Module* module = model->createModule();
		APP->engine->addModule(module);
		mw = model->createModuleWidget(module);
		place(mw, anchor);
		theRack()->addModule(mw);
		noteModuleAdded(module->id);
		g_lastAdded = module->id;
	}

	json_t* before = mw->toJson();
	json_t* after = json_deep_copy(before);
	json_t* data = json_object_get(after, "data");
	std::vector<int> cells = padCellsOf(mw->module);
	if (isNew) {
		// The order of the outputs is that of CONTROLS.
		cells.assign(CELLS, -1);
		for (size_t k = 0; k < sizeof CONTROLS / sizeof CONTROLS[0]; k++)
			cells[k] = CONTROLS[k].cc;
		json_t* midi = json_object();
		json_object_set_new(midi, "driver", json_integer(PAD_DRIVER));
		json_object_set_new(midi, "deviceName", json_string(PAD_DEVICE));
		json_object_set_new(midi, "channel", json_integer(-1));
		json_object_set_new(data, "midi", midi);
		json_object_set_new(data, "smooth", json_true());
	}
	for (const Control* c : wanted) {
		if (std::find(cells.begin(), cells.end(), c->cc) != cells.end())
			continue;
		// An output that nothing hangs on, and that is not one of the controls.
		int free = -1;
		for (int k = CELLS - 1; k >= 0; k--) {
			bool control = false;
			for (const Control& other : CONTROLS)
				control = control || other.cc == cells[k];
			app::PortWidget* port = mw->getOutput(k);
			if (!control && port && theRack()->getCompleteCablesOnPort(port).empty())
				free = k;
		}
		if (free < 0) {
			json_decref(before);
			json_decref(after);
			throw OpError{"All outputs of " + nameOf(mw) + " are in use."};
		}
		cells[free] = c->cc;
	}
	json_t* ccs = json_array();
	for (int cc : cells)
		json_array_append_new(ccs, json_integer(cc));
	json_object_set_new(data, "ccs", ccs);
	mw->fromJson(after);

	if (isNew) {
		history::ModuleAdd* h = new history::ModuleAdd;
		h->setModule(mw);
		group()->push(h);
		json_decref(before);
		json_decref(after);
	}
	else if (json_equal(before, after)) {
		json_decref(before);
		json_decref(after);
	}
	else {
		history::ModuleChange* h = new history::ModuleChange;
		h->moduleId = mw->module->id;
		h->oldModuleJ = before;
		h->newModuleJ = after;
		group()->push(h);
	}
	gamepadNavShow(mw->module->id);
	return (isNew ? "Added " : "Already there: ") + nameOf(mw) + " (" + modelKey(mw->model) + ")\n" + describe(mw->module, mw);
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
	gamepadNavShow(in->module->id);
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
		gamepadNavShow(m->id);
	}
	return nameOf(mw) + " " + paramText(pq);
}


/** What Rack's own actions push on the history while this is about, taken off
it again and put into this turn's action. */
struct Absorb {
	history::Action* last = NULL;

	Absorb() {
		history::State* h = APP->history;
		if (h->actionIndex > 0)
			last = h->actions[h->actionIndex - 1];
	}

	/** Returns how many there were. */
	int finish() {
		history::State* h = APP->history;
		// By what came before, not by a number: the history drops its oldest.
		int base = h->actionIndex;
		while (base > 0 && h->actions[base - 1] != last)
			base--;
		std::vector<history::Action*> got(h->actions.begin() + base, h->actions.begin() + h->actionIndex);
		if (got.empty())
			return 0;
		h->actions.resize(base);
		h->actionIndex = base;
		history::ComplexAction* g = group();
		for (history::Action* a : got)
			g->push(a);
		return (int) got.size();
	}
};


/* ---- A module's menu ---- */

/** One row that can be chosen. The menus it hangs from are this one's to
delete. */
struct MenuRow {
	std::vector<std::unique_ptr<ui::Menu>> menus;
	widget::Widget* widget = NULL;
};

static const int MAX_MENU_ROWS = 260;


static bool choosable(widget::Widget* w) {
	return dynamic_cast<ui::MenuItem*>(w) || dynamic_cast<ui::Slider*>(w) || dynamic_cast<ui::TextField*>(w);
}


static void menuText(ui::Menu* menu, const std::string& prefix, int depth, int& rows, std::string& out) {
	const std::string indent(2 + depth * 2, ' ');
	int n = 0;
	for (widget::Widget* c : menu->children) {
		if (rows > MAX_MENU_ROWS)
			return;
		if (ui::MenuLabel* label = dynamic_cast<ui::MenuLabel*>(c)) {
			if (!label->text.empty())
				out += indent + "(" + label->text + ")\n";
			continue;
		}
		if (!choosable(c))
			continue;
		std::string id = prefix + string::f("%d", ++n);
		if (++rows > MAX_MENU_ROWS) {
			out += indent + "... and more\n";
			return;
		}
		if (ui::Slider* slider = dynamic_cast<ui::Slider*>(c)) {
			Quantity* q = slider->quantity;
			if (q)
				out += indent + id + " slider \"" + q->getLabel() + "\": " + q->getDisplayValueString() + q->getUnit()
					+ " [value " + number(q->getValue()) + " in " + number(q->getMinValue()) + ".." + number(q->getMaxValue()) + "]\n";
			continue;
		}
		if (ui::TextField* field = dynamic_cast<ui::TextField*>(c)) {
			out += indent + id + " text \"" + field->text + "\"\n";
			continue;
		}
		ui::MenuItem* item = dynamic_cast<ui::MenuItem*>(c);
		// Where many of them work out their check mark.
		item->step();
		out += indent + id + " \"" + item->text + "\"";
		std::unique_ptr<ui::Menu> child(item->disabled ? NULL : item->createChildMenu());
		std::string right = string::trim(item->rightText);
		if (child) {
			// The arrow of a submenu says nothing; what may stand before it does.
			size_t arrow = right.find(RIGHT_ARROW);
			if (arrow != std::string::npos)
				right = string::trim(right.substr(0, arrow));
		}
		if (!right.empty())
			out += " " + right;
		if (item->disabled)
			out += " (cannot be chosen now)";
		out += "\n";
		if (child) {
			if (depth < 4)
				menuText(child.get(), id + ".", depth + 1, rows, out);
			else
				out += indent + "  ...\n";
		}
	}
}


struct Preset {
	std::string path;
	std::string name;
};


static void findPresets(const std::string& dir, const std::string& prefix, std::vector<Preset>& out) {
	if (!system::isDirectory(dir))
		return;
	std::vector<std::string> entries = system::getEntries(dir);
	std::sort(entries.begin(), entries.end());
	for (const std::string& path : entries) {
		if (out.size() >= 120)
			return;
		std::string name = system::getStem(path);
		if (system::isDirectory(path)) {
			findPresets(path, prefix + name + " / ", out);
			continue;
		}
		if (system::getExtension(path) != ".vcvm" || name == "template")
			continue;
		// "001_Name" is how they are kept in order.
		size_t digits = name.find_first_not_of("0123456789");
		if (digits != std::string::npos && digits > 0 && name[digits] == '_')
			name = name.substr(digits + 1);
		out.push_back(Preset{path, prefix + name});
	}
}


static std::vector<Preset> presetsOf(app::ModuleWidget* mw) {
	std::vector<Preset> out;
	findPresets(mw->model->getUserPresetDirectory(), "", out);
	findPresets(mw->model->getFactoryPresetDirectory(), "", out);
	return out;
}


static std::unique_ptr<ui::Menu> menuOf(app::ModuleWidget* mw) {
	std::unique_ptr<ui::Menu> menu(new ui::Menu);
	mw->appendContextMenu(menu.get());
	return menu;
}


static std::string opMenu(json_t* req) {
	app::ModuleWidget* mw = moduleArg(req, "module");
	std::string out;
	int rows = 0;
	menuText(menuOf(mw).get(), "", 0, rows, out);
	out = rows > 0 ? "Menu of " + nameOf(mw) + ":\n" + out : nameOf(mw) + " has no menu of its own.\n";
	std::vector<Preset> presets = presetsOf(mw);
	for (size_t i = 0; i < presets.size(); i++)
		out += string::f("%s  p%d \"%s\"\n", i == 0 ? "Presets:\n" : "", (int) i + 1, presets[i].name.c_str());
	return out;
}


/** The row an id like "2.1.3" means. */
static MenuRow findRow(app::ModuleWidget* mw, const std::string& id) {
	MenuRow row;
	row.menus.push_back(menuOf(mw));
	ui::Menu* menu = row.menus.back().get();
	std::vector<std::string> parts = string::split(id, ".");
	for (size_t i = 0; i < parts.size(); i++) {
		int want = std::atoi(parts[i].c_str());
		row.widget = NULL;
		for (widget::Widget* c : menu->children) {
			if (choosable(c) && --want == 0) {
				row.widget = c;
				break;
			}
		}
		if (!row.widget)
			throw OpError{"There is no row " + id + " in the menu of " + nameOf(mw) + ". Read the menu."};
		ui::MenuItem* item = dynamic_cast<ui::MenuItem*>(row.widget);
		ui::Menu* child = item && !item->disabled ? item->createChildMenu() : NULL;
		if (child)
			row.menus.emplace_back(child);
		if (i + 1 == parts.size()) {
			if (child)
				throw OpError{"Row " + id + " opens a submenu: choose a row in it."};
		}
		else if (!child) {
			throw OpError{"Row " + id + ": there is no submenu there. Read the menu."};
		}
		menu = child;
	}
	return row;
}


static std::string opChoose(json_t* req) {
	checkLock(2);
	app::ModuleWidget* mw = moduleArg(req, "module");
	int64_t id = mw->module->id;
	std::string name = nameOf(mw);
	json_t* itemJ = json_object_get(req, "item");
	std::string itemId = json_is_integer(itemJ) ? string::f("%d", (int) json_integer_value(itemJ))
		: json_is_string(itemJ) ? json_string_value(itemJ) : "";
	json_t* value = json_object_get(req, "value");
	json_t* text = json_object_get(req, "text");

	Absorb absorb;
	if (!itemId.empty() && (itemId[0] == 'p' || itemId[0] == 'P')) {
		std::vector<Preset> presets = presetsOf(mw);
		int n = std::atoi(itemId.c_str() + 1);
		if (n < 1 || n > (int) presets.size())
			throw OpError{name + " has no preset " + itemId + "."};
		mw->loadAction(presets[n - 1].path);
		absorb.finish();
		return name + ": loaded the preset \"" + presets[n - 1].name + "\".\n" + describe(mw->module, mw);
	}

	MenuRow row = findRow(mw, itemId);
	std::string label;
	json_t* before = mw->toJson();
	if (ui::Slider* slider = dynamic_cast<ui::Slider*>(row.widget)) {
		if (!slider->quantity || !json_is_number(value)) {
			json_decref(before);
			throw OpError{"Row " + itemId + " is a slider: give a value."};
		}
		slider->quantity->setValue((float) json_number_value(value));
		label = slider->quantity->getLabel() + ": " + slider->quantity->getDisplayValueString() + slider->quantity->getUnit();
	}
	else if (ui::TextField* field = dynamic_cast<ui::TextField*>(row.widget)) {
		if (!json_is_string(text)) {
			json_decref(before);
			throw OpError{"Row " + itemId + " is a text: give one."};
		}
		field->setText(json_string_value(text));
		widget::EventContext context;
		widget::Widget::ActionEvent e;
		e.context = &context;
		field->onAction(e);
		label = field->text;
	}
	else {
		ui::MenuItem* item = dynamic_cast<ui::MenuItem*>(row.widget);
		if (item->disabled) {
			json_decref(before);
			throw OpError{"Row " + itemId + " \"" + item->text + "\" cannot be chosen now."};
		}
		label = item->text;
		item->doAction(true);
	}
	int own = absorb.finish();
	// It may have taken the module away, or put another in its place.
	mw = theRack()->getModule(id);
	if (own == 0 && mw) {
		json_t* after = mw->toJson();
		if (json_equal(before, after)) {
			json_decref(before);
			json_decref(after);
		}
		else {
			history::ModuleChange* h = new history::ModuleChange;
			h->moduleId = id;
			h->oldModuleJ = before;
			h->newModuleJ = after;
			group()->push(h);
		}
	}
	else {
		json_decref(before);
	}
	if (mw)
		gamepadNavShow(id);
	return name + ": chose \"" + label + "\".";
}


static std::string opAct(json_t* req) {
	app::ModuleWidget* mw = moduleArg(req, "module");
	const char* whatJ = json_string_value(json_object_get(req, "what"));
	std::string what = whatJ ? whatJ : "";
	std::string name = nameOf(mw);
	bool layout = what == "duplicate" || what == "duplicate_with_cables" || what == "unplug_all";
	checkLock(layout ? 1 : 2);
	Absorb absorb;
	if (what == "bypass" || what == "unbypass") {
		if (mw->module->isBypassed() == (what == "bypass"))
			return name + " was already so.";
		mw->bypassAction(what == "bypass");
	}
	else if (what == "initialize") mw->resetAction();
	else if (what == "randomize") mw->randomizeAction();
	else if (what == "unplug_all") mw->disconnectAction();
	else if (what == "duplicate" || what == "duplicate_with_cables") {
		std::set<int64_t> known;
		for (app::ModuleWidget* o : theRack()->getModules())
			known.insert(o->module->id);
		mw->cloneAction(what == "duplicate_with_cables");
		absorb.finish();
		for (app::ModuleWidget* o : theRack()->getModules()) {
			if (known.count(o->module->id))
				continue;
			g_lastAdded = o->module->id;
			noteModuleAdded(o->module->id);
			gamepadNavShow(o->module->id);
			return "Duplicated " + name + " as " + nameOf(o) + ".";
		}
		return "Rack would not duplicate it.";
	}
	else throw OpError{"There is no action \"" + what + "\"."};
	absorb.finish();
	gamepadNavShow(mw->module->id);
	return name + ": " + what + ".";
}


static std::string opMove(json_t* req) {
	checkLock(1);
	app::ModuleWidget* mw = moduleArg(req, "module");
	app::ModuleWidget* other = moduleArg(req, "other");
	const char* whereJ = json_string_value(json_object_get(req, "where"));
	std::string where = whereJ ? whereJ : "";
	if (mw == other)
		throw OpError{"A module cannot be moved beside itself."};
	app::RackWidget* rack = theRack();
	rack->updateModuleOldPositions();
	math::Vec at = other->box.pos;
	if (where == "right_of" || where == "left_of") {
		// In between: whatever stands there makes room.
		math::Vec pos(where == "right_of" ? other->box.getRight() : at.x, at.y);
		if (settings::squeezeModules)
			rack->setModulePosSqueeze(mw, pos);
		else
			rack->setModulePosForce(mw, pos);
	}
	else if (where == "below" || where == "above") {
		rack->setModulePosNearest(mw, math::Vec(at.x, at.y + (where == "below" ? 1 : -1) * app::RACK_GRID_HEIGHT));
	}
	else throw OpError{"where is right_of, left_of, below or above."};
	rack->updateExpanders();
	group()->push(rack->getModuleDragAction());
	gamepadNavShow(mw->module->id);
	return "Moved " + nameOf(mw) + " " + where + " " + nameOf(other) + ".";
}


/* ---- The patch as a file ---- */

/** What was open before the chat opened something else. */
static const char LAST_CLOSED[] = "Last closed by chat";


static std::string patchesDir() {
	return asset::user("patches");
}


static std::string patchPath(const std::string& given) {
	std::string name = string::trim(given);
	if (string::endsWith(string::lowercase(name), ".vcv"))
		name = name.substr(0, name.size() - 4);
	if (name.empty() || name[0] == '.' || name.find('/') != std::string::npos || name.size() > 100)
		throw OpError{"\"" + given + "\" is no name for a patch."};
	return system::join(patchesDir(), name + ".vcv");
}


static std::string openName() {
	return APP->patch->path.empty() ? "" : system::getStem(APP->patch->path);
}


static std::string opPatches() {
	std::string out = openName().empty() ? "The open patch has no name yet" : "Open: \"" + openName() + "\"";
	out += APP->history->isSaved() ? ", with no unsaved changes.\n" : ", with unsaved changes.\n";
	std::vector<std::string> entries = system::getEntries(patchesDir());
	std::sort(entries.begin(), entries.end());
	out += "Saved patches:\n";
	for (const std::string& path : entries) {
		if (system::getExtension(path) == ".vcv")
			out += "  " + system::getStem(path) + "\n";
	}
	return out;
}


static std::string opSave(json_t* req) {
	const char* nameJ = json_string_value(json_object_get(req, "name"));
	std::string path = APP->patch->path;
	bool named = nameJ && *nameJ;
	if (named)
		path = patchPath(nameJ);
	else if (path.empty())
		throw OpError{"The patch has no name yet: give one, or ask the user for one."};
	if (named && system::getFilename(path) != system::getFilename(APP->patch->path) && system::exists(path)
			&& !json_is_true(json_object_get(req, "overwrite")))
		throw OpError{"There is already a patch \"" + system::getStem(path) + "\". Ask the user before overwriting it."};
	system::createDirectory(patchesDir());
	// As Manager::saveDialog(): before saving, so the file does not say it is
	// unsaved.
	APP->history->setSaved();
	APP->patch->save(path);
	if (named) {
		APP->patch->path = path;
		settings::lastPatchDirectory = system::getDirectory(path);
		APP->patch->pushRecentPath(path);
	}
	return "Saved as \"" + system::getStem(path) + "\".";
}


static std::string opOpen(json_t* req) {
	checkLock(1);
	const char* nameJ = json_string_value(json_object_get(req, "name"));
	const char* unsavedJ = json_string_value(json_object_get(req, "unsaved"));
	std::string unsaved = unsavedJ ? unsavedJ : "";
	std::string path;
	if (nameJ && *nameJ) {
		path = patchPath(nameJ);
		if (!system::isFile(path))
			throw OpError{"There is no patch \"" + std::string(nameJ) + "\". list_patches has the names."};
	}
	std::string kept;
	if (!APP->history->isSaved() && unsaved != "discard") {
		if (unsaved != "keep")
			throw OpError{"The open patch has unsaved changes. Ask the user whether to save it first, to keep a copy of it (unsaved: keep) or to drop the changes (unsaved: discard)."};
		char stamp[32];
		std::time_t now = std::time(NULL);
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
		kept = (openName().empty() ? std::string("Untitled") : openName()) + " kept " + stamp;
		APP->patch->save(patchPath(kept));
	}
	// Whatever was said: what Rack takes for saved may be a copy under another
	// name, and what was meant by dropping it may not have been this. Aside
	// first, in case it is the one being opened.
	std::string last = patchPath(LAST_CLOSED);
	std::string aside = last + ".new";
	bool empty = theRack()->getModules().empty();
	if (!empty)
		APP->patch->save(aside);
	if (path.empty()) {
		APP->patch->loadTemplate();
	}
	else {
		APP->patch->load(path);
		APP->history->setSaved();
		// Not the one kept aside: that name is about to mean something else.
		APP->patch->path = path == last ? "" : path;
		if (path != last)
			APP->patch->pushRecentPath(path);
	}
	if (!empty) {
		system::remove(last);
		system::rename(aside, last);
	}
	g_numberOf.clear();
	g_idOf.clear();
	g_nextNumber = 1;
	g_lastAdded = -1;
	return (path.empty() ? std::string("Opened a new patch.") : "Opened \"" + system::getStem(path) + "\".")
		+ (empty ? "" : " What was open is kept as \"" + (kept.empty() ? std::string(LAST_CLOSED) : kept) + "\".") + "\n" + opPatch();
}


static std::string opPlay(json_t* req) {
	bool on = json_is_true(json_object_get(req, "on"));
	transportSetPaused(!on);
	return on ? "Playing." : "Paused.";
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
	bool begun = false;
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
		else if (op == "controls") text = opControls(req);
		else if (op == "connect") text = opConnect(req);
		else if (op == "disconnect") text = opDisconnect(req);
		else if (op == "set") text = opSet(req);
		else if (op == "menu") text = opMenu(req);
		else if (op == "choose") text = opChoose(req);
		else if (op == "act") text = opAct(req);
		else if (op == "move") text = opMove(req);
		else if (op == "play") text = opPlay(req);
		else if (op == "patches") text = opPatches();
		else if (op == "save") text = opSave(req);
		else if (op == "open") text = opOpen(req);
		else if (op == "undo") text = opUndo();
		else if (op.compare(0, 5, "made_") == 0) text = madeOp(op, req);
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
			r->begun = true;
			g_answered.notify_all();
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
	// Long once it has been begun: a row of a menu may ask the user for a file.
	if (!g_answered.wait_for(lock, std::chrono::seconds(8), [&] { return r->done || r->begun; })
			|| !g_answered.wait_for(lock, std::chrono::seconds(110), [&] { return r->done; })) {
		r->dropped = true;
		if (r->begun)
			return answer(false, "It was begun but is not finished: the user is perhaps being asked something.");
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

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_ChatPanel_nativeChatInset(JNIEnv*, jobject, jint right, jint bottom) {
	rackdroid::gamepadNavPanelInset((int) right, (int) bottom);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_ChatPanel_nativeChatSticks(JNIEnv*, jobject, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat l2, jfloat r2) {
	const float axes[rackdroid::PAD_AXES] = {lx, ly, rx, ry, l2, r2};
	rackdroid::gamepadPanelAxes(axes);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_ChatPanel_nativeChatGate(JNIEnv*, jobject, jboolean down) {
	rackdroid::gamepadPanelButton(rackdroid::PAD_L3, down);
}
