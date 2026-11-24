/* See made_modules.hpp.
 *
 * Requests, as patch_ops.cpp passes them on ({"op": ...}):
 *
 *   made_load    slug, version, meta (made.json), dir, soname, marker; or, for a
 *                stand-in, no soname and "why"
 *   made_off     slug, why: the stand-in takes over
 *   made_drop    slug: gone from the list of modules; refused while in the patch
 *   made_status  which are in the patch, and which of those are stopped
 *   made_faults  what went wrong since the start, with places in the libraries
 *
 * The app's Java side brings the library in first (System.load), as for any
 * pack; see static_plugins.cpp.
 */
#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <vector>

#include <dlfcn.h>
#include <jni.h>
#include <android/log.h>

#include <rack.hpp>

#include "jni_bridge.hpp"
#include "made_modules.hpp"
#include "module_guard.hpp"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "rackdroid.made", __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, "rackdroid.made", __VA_ARGS__)


namespace rackdroid {


using namespace rack;


namespace {

const char PLUGIN[] = "NovaMade";

struct ParamMeta {
	std::string name;
	float min = 0.f;
	float max = 1.f;
	float standard = 0.f;
};

/** One library. Never unloaded: modules and the history may point into it. */
struct Code {
	int version = 0;
	plugin::Model* model = NULL;
	int (*stopped)(engine::Module*) = NULL;
};

struct MadeModel : plugin::Model {
	/** What runs, or NULL for the stand-in. */
	Code* code = NULL;
	/** By the library's name. */
	std::map<std::string, Code*> codes;
	int version = 0;
	std::string why;
	std::vector<ParamMeta> params;
	std::vector<std::string> inputs;
	std::vector<std::string> outputs;
	int lights = 0;
	int width = 8;

	engine::Module* createModule() override;
	app::ModuleWidget* createModuleWidget(engine::Module* m) override;
};

struct Fault {
	std::string slug;
	int version;
	std::string where;
	std::string what;
	uintptr_t offset;
	double time;
};

/** By its library's name in the app, "Folder 3". */
struct Tag {
	std::string slug;
	int version;
};

std::list<Fault> g_faults;
/** The last thing that went wrong in a module that is still there. */
std::map<const engine::Module*, std::string> g_why;


plugin::Plugin* thePlugin() {
	if (plugin::Plugin* p = plugin::getPlugin(PLUGIN))
		return p;
	plugin::Plugin* p = new plugin::Plugin;
	p->slug = PLUGIN;
	p->name = "Nova Made";
	p->brand = "Nova Made";
	p->version = "2.0.0";
	p->license = "GPL-3.0-or-later";
	p->description = "Made on this device, from the chat";
	plugin::plugins.push_back(p);
	return p;
}


MadeModel* findModel(const std::string& slug) {
	plugin::Plugin* p = plugin::getPlugin(PLUGIN);
	return p ? dynamic_cast<MadeModel*>(p->getModel(slug)) : NULL;
}


/** Runs f in the guard. */
template <class F>
bool guarded(const void* owner, GuardWhere where, F f) {
	return nova_guard_call([](void* p) { (*(F*) p)(); }, &f, owner, where) == 0;
}


/* ---- The stand-in ---- */

struct StandIn : engine::Module {
	json_t* data = NULL;
	std::string why;

	StandIn(MadeModel* model) {
		why = model->why;
		config((int) model->params.size(), (int) model->inputs.size(), (int) model->outputs.size(), model->lights);
		for (size_t i = 0; i < model->params.size(); i++)
			configParam((int) i, model->params[i].min, model->params[i].max, model->params[i].standard, model->params[i].name);
		for (size_t i = 0; i < model->inputs.size(); i++)
			configInput((int) i, model->inputs[i]);
		for (size_t i = 0; i < model->outputs.size(); i++)
			configOutput((int) i, model->outputs[i]);
	}
	~StandIn() {
		json_decref(data);
	}
	/** What the module kept, for when it is back. */
	json_t* dataToJson() override {
		return data ? json_deep_copy(data) : NULL;
	}
	void dataFromJson(json_t* j) override {
		json_decref(data);
		data = json_deep_copy(j);
	}
};


/** A panel for a module whose own cannot be made. */
struct StandInWidget : app::ModuleWidget {
	std::string title;

	StandInWidget(engine::Module* m, MadeModel* model) {
		setModule(m);
		title = model->name;
		box.size = math::Vec(std::max(model->width, 3) * app::RACK_GRID_WIDTH, app::RACK_GRID_HEIGHT);
		int params = m ? (int) m->params.size() : (int) model->params.size();
		int inputs = m ? (int) m->inputs.size() : (int) model->inputs.size();
		int outputs = m ? (int) m->outputs.size() : (int) model->outputs.size();
		int columns = std::max(1, (int) ((box.size.x - 6.f) / 27.f));
		auto at = [&](int i, float y) {
			return math::Vec((i % columns + 0.5f) * (box.size.x / columns), y + 27.f * (i / columns));
		};
		for (int i = 0; i < params; i++)
			addParam(createParamCentered<componentlibrary::Trimpot>(at(i, 52.f), m, i));
		int rows = (inputs + columns - 1) / columns + (outputs + columns - 1) / columns;
		float y = app::RACK_GRID_HEIGHT - 12.f - 27.f * rows;
		for (int i = 0; i < inputs; i++)
			addInput(createInputCentered<componentlibrary::PJ301MPort>(at(i, y), m, i));
		y += 27.f * ((inputs + columns - 1) / columns);
		for (int i = 0; i < outputs; i++)
			addOutput(createOutputCentered<componentlibrary::PJ301MPort>(at(i, y), m, i));
	}

	void draw(const DrawArgs& args) override {
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x3a, 0x22, 0x24));
		nvgFill(args.vg);
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0.5f, 0.5f, box.size.x - 1.f, box.size.y - 1.f);
		nvgStrokeColor(args.vg, nvgRGB(0xc6, 0x28, 0x28));
		nvgStrokeWidth(args.vg, 1.f);
		nvgStroke(args.vg);
		if (APP->window->uiFont)
			nvgFontFaceId(args.vg, APP->window->uiFont->handle);
		nvgFontSize(args.vg, 13.f);
		nvgTextAlign(args.vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
		nvgFillColor(args.vg, nvgRGB(255, 255, 255));
		nvgSave(args.vg);
		nvgIntersectScissor(args.vg, 2, 0, box.size.x - 4, box.size.y);
		nvgText(args.vg, box.size.x / 2.f, 18.f, title.c_str(), NULL);
		nvgRestore(args.vg);
		nvgFillColor(args.vg, nvgRGB(0xff, 0x8a, 0x80));
		nvgFontSize(args.vg, std::fmin(13.f, box.size.x / 4.f));
		nvgText(args.vg, box.size.x / 2.f, 34.f, "STOPPED", NULL);
		ModuleWidget::draw(args);
	}
};


/* ---- The models ---- */

engine::Module* MadeModel::createModule() {
	engine::Module* m = NULL;
	if (code) {
		plugin::Model* inner = code->model;
		if (!guarded(this, GUARD_MAKE, [&] { m = inner->createModule(); }))
			m = NULL;
	}
	if (!m) {
		StandIn* s = new StandIn(this);
		if (s->why.empty())
			s->why = "it cannot be made";
		m = s;
	}
	m->model = this;
	return m;
}


app::ModuleWidget* MadeModel::createModuleWidget(engine::Module* m) {
	app::ModuleWidget* mw = NULL;
	if (code && !dynamic_cast<StandIn*>(m)) {
		// Its own model checks that the module is of it.
		plugin::Model* inner = code->model;
		if (m)
			m->model = inner;
		if (!guarded(this, GUARD_WIDGET, [&] { mw = inner->createModuleWidget(m); }))
			mw = NULL;
		if (m)
			m->model = this;
	}
	if (!mw)
		mw = new StandInWidget(m, this);
	mw->model = this;
	return mw;
}


std::string text(json_t* o, const char* key) {
	const char* s = json_string_value(json_object_get(o, key));
	return s ? s : "";
}


std::vector<std::string> names(json_t* list) {
	std::vector<std::string> out;
	size_t i;
	json_t* j;
	json_array_foreach(list, i, j) {
		const char* s = json_string_value(json_is_object(j) ? json_object_get(j, "name") : j);
		out.push_back(s ? s : "");
	}
	return out;
}


/** The modules of the patch that are of this model. */
std::vector<app::ModuleWidget*> inPatch(MadeModel* model) {
	std::vector<app::ModuleWidget*> out;
	for (app::ModuleWidget* mw : APP->scene->rack->getModules()) {
		if (mw->model == model && mw->module)
			out.push_back(mw);
	}
	return out;
}


/** Takes the modules of this model out of the patch and puts them back, as the
model makes them now. Not a step of Undo: the module ids stay, and with them
whatever the history knows of. */
std::string exchange(MadeModel* model) {
	app::RackWidget* rack = APP->scene->rack;
	std::vector<app::ModuleWidget*> all = inPatch(model);
	int lost = 0;
	for (app::ModuleWidget* mw : all) {
		std::vector<history::CableRemove*> cables;
		for (app::PortWidget* pw : mw->getPorts()) {
			for (app::CableWidget* cw : rack->getCompleteCablesOnPort(pw)) {
				history::CableRemove* h = new history::CableRemove;
				h->setCable(cw);
				cables.push_back(h);
				rack->removeCable(cw);
				delete cw;
			}
		}
		history::ModuleRemove* h = new history::ModuleRemove;
		h->setModule(mw);
		engine::Module* old = mw->module;
		int64_t id = old->id;
		bool selected = rack->isSelected(mw);
		rack->removeModule(mw);
		APP->engine->removeModule(old);
		mw->module = NULL;
		delete mw;
		g_why.erase(old);
		// Its destructor is its own code too, after perhaps having gone wrong.
		guarded(model, GUARD_REMOVE, [&] { delete old; });

		h->undo();
		delete h;
		app::ModuleWidget* now = rack->getModule(id);
		if (!now)
			continue;
		// Wider than it was.
		rack->setModulePosForce(now, now->box.pos);
		if (selected)
			rack->select(now);
		for (history::CableRemove* c : cables) {
			bool in = c->inputModuleId == id && c->inputId >= (int) now->module->inputs.size();
			bool out = c->outputModuleId == id && c->outputId >= (int) now->module->outputs.size();
			if (in || out)
				lost++;
			else
				c->undo();
			delete c;
		}
	}
	if (all.empty())
		return "";
	rack->updateExpanders();
	std::string s = string::f("%d in the patch %s exchanged for it, with %s cables.", (int) all.size(), all.size() == 1 ? "was" : "were",
		all.size() == 1 ? "its" : "their");
	if (lost)
		s += string::f(" %d cables were lost: their jacks are no longer there.", lost);
	return s;
}


Code* loadCode(MadeModel* model, int version, const std::string& dir, const std::string& soname) {
	auto it = model->codes.find(soname);
	if (it != model->codes.end())
		return it->second;
	// By its path: the versions all have the same name inside.
	void* handle = dlopen(system::join(dir, soname).c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
	if (!handle)
		throw Exception("its library is not there: %s", dlerror());
	void (*init)(plugin::Plugin*) = (void (*)(plugin::Plugin*)) dlsym(handle, "init");
	int (*stopped)(engine::Module*) = (int (*)(engine::Module*)) dlsym(handle, "nova_stopped");
	if (!init || !stopped)
		throw Exception("its library is not one of a made module");
	guardAddCode((const void*) init, model->slug + string::f(" %d", version));
	plugin::Plugin* p = new plugin::Plugin;
	p->slug = PLUGIN;
	p->version = "2.0.0";
	p->path = dir;
	if (!guarded(model, GUARD_MAKE, [&] { init(p); }) || p->models.size() != 1)
		throw Exception("it cannot be started");
	Code* code = new Code;
	code->version = version;
	code->model = p->models.front();
	code->model->name = model->name;
	code->stopped = stopped;
	model->codes[soname] = code;
	return code;
}


std::string opLoad(json_t* req, bool live) {
	std::string slug = text(req, "slug");
	json_t* meta = json_object_get(req, "meta");
	if (slug.empty() || !json_is_object(meta))
		throw Exception("no slug or no meta");
	guardInstall(text(req, "marker"));
	MadeModel* model = findModel(slug);
	bool isNew = !model;
	if (isNew) {
		model = new MadeModel;
		model->slug = slug;
		model->name = slug;
	}
	std::string error;
	Code* code = NULL;
	int version = (int) json_integer_value(json_object_get(req, "version"));
	if (!text(req, "soname").empty()) {
		try {
			code = loadCode(model, version, text(req, "dir"), text(req, "soname"));
		}
		catch (std::exception& e) {
			error = e.what();
			// Nothing changes; only a patch that is about to be opened needs a
			// stand-in for what does not load.
			if (live)
				throw;
		}
	}
	std::string name = text(meta, "name");
	model->name = name.empty() ? slug : name;
	model->description = text(meta, "description");
	model->params.clear();
	size_t i;
	json_t* j;
	json_array_foreach(json_object_get(meta, "params"), i, j) {
		ParamMeta p;
		p.name = text(j, "name");
		p.min = json_number_value(json_object_get(j, "min"));
		p.max = json_number_value(json_object_get(j, "max"));
		p.standard = json_number_value(json_object_get(j, "default"));
		if (!(p.max > p.min))
			p.max = p.min + 1.f;
		model->params.push_back(p);
	}
	model->inputs = names(json_object_get(meta, "inputs"));
	model->outputs = names(json_object_get(meta, "outputs"));
	model->lights = (int) json_integer_value(json_object_get(meta, "lights"));
	model->width = (int) json_integer_value(json_object_get(meta, "width"));
	model->version = version;
	model->code = code;
	model->why = code ? "" : error.empty() ? text(req, "why") : error;
	if (code)
		code->model->name = model->name;
	if (isNew)
		thePlugin()->addModel(model);
	json_t* m = json_object();
	json_object_set_new(m, "name", json_string(model->name.c_str()));
	json_object_set_new(m, "description", json_string(model->description.c_str()));
	json_object_set(m, "tags", json_object_get(meta, "tags"));
	model->fromJson(m);
	json_decref(m);
	LOGI("%s version %d %s%s", slug.c_str(), version, code ? "loaded" : "stands still: ", model->why.c_str());
	if (!error.empty())
		throw Exception("%s", error.c_str());
	return live ? exchange(model) : "";
}


std::string opOff(json_t* req) {
	MadeModel* model = findModel(text(req, "slug"));
	if (!model)
		throw Exception("there is no such module");
	model->code = NULL;
	model->why = text(req, "why");
	return exchange(model);
}


std::string opDrop(json_t* req) {
	MadeModel* model = findModel(text(req, "slug"));
	if (!model)
		return "";
	int n = (int) inPatch(model).size();
	if (n)
		throw Exception("%d of it %s in the patch. Take %s out first.", n, n == 1 ? "is" : "are", n == 1 ? "it" : "them");
	// The history may still hold it.
	thePlugin()->models.remove(model);
	model->code = NULL;
	model->why = "it has been removed";
	return "";
}


std::string opStatus() {
	json_t* out = json_object();
	plugin::Plugin* p = plugin::getPlugin(PLUGIN);
	if (p) {
		for (plugin::Model* m : p->models) {
			MadeModel* model = dynamic_cast<MadeModel*>(m);
			if (!model)
				continue;
			int n = 0;
			std::string stopped;
			for (app::ModuleWidget* mw : inPatch(model)) {
				n++;
				std::string s = madeStatus(mw->module);
				if (!s.empty())
					stopped = s;
			}
			json_t* o = json_object();
			json_object_set_new(o, "in_patch", json_integer(n));
			json_object_set_new(o, "running", json_boolean(model->code != NULL));
			json_object_set_new(o, "version", json_integer(model->version));
			json_object_set_new(o, "stopped", json_string(stopped.c_str()));
			json_object_set_new(out, model->slug.c_str(), o);
		}
	}
	char* dump = json_dumps(out, JSON_COMPACT);
	std::string s = dump ? dump : "{}";
	free(dump);
	json_decref(out);
	return s;
}


std::string opFaults() {
	json_t* out = json_array();
	for (const Fault& f : g_faults) {
		json_t* o = json_object();
		json_object_set_new(o, "slug", json_string(f.slug.c_str()));
		json_object_set_new(o, "version", json_integer(f.version));
		json_object_set_new(o, "where", json_string(f.where.c_str()));
		json_object_set_new(o, "what", json_string(f.what.c_str()));
		json_object_set_new(o, "offset", json_integer((json_int_t) f.offset));
		json_array_append_new(out, o);
	}
	char* dump = json_dumps(out, JSON_COMPACT);
	std::string s = dump ? dump : "[]";
	free(dump);
	json_decref(out);
	return s;
}

} // namespace


std::string madeStatus(engine::Module* module) {
	MadeModel* model = module ? dynamic_cast<MadeModel*>(module->model) : NULL;
	if (!model)
		return "";
	if (StandIn* s = dynamic_cast<StandIn*>(module))
		return "STOPPED: " + s->why;
	for (auto& c : model->codes) {
		if (c.second != model->code || !c.second->stopped(module))
			continue;
		auto it = g_why.find(module);
		return "STOPPED by an error in its code" + (it == g_why.end() ? "" : ": " + it->second);
	}
	return "";
}


std::string madeOp(const std::string& op, json_t* req) {
	if (op == "made_load") return opLoad(req, true);
	if (op == "made_off") return opOff(req);
	if (op == "made_drop") return opDrop(req);
	if (op == "made_status") return opStatus();
	if (op == "made_faults") return opFaults();
	throw Exception("There is no operation \"%s\".", op.c_str());
}


void madeModulesStep() {
	GuardFault f;
	while (guardNextFault(f)) {
		Fault fault;
		fault.offset = f.offset;
		fault.where = guardWhereName(f.where);
		fault.what = f.what;
		fault.version = 0;
		fault.time = system::getTime();
		std::string name;
		// The owner may have been deleted since: only looked for, not used.
		plugin::Plugin* p = plugin::getPlugin(PLUGIN);
		if (p) {
			for (plugin::Model* m : p->models) {
				MadeModel* model = dynamic_cast<MadeModel*>(m);
				if (!model)
					continue;
				bool mine = f.owner == (const void*) model;
				for (app::ModuleWidget* mw : inPatch(model)) {
					if ((const void*) mw->module == f.owner) {
						mine = true;
						g_why[mw->module] = fault.what + " " + fault.where;
					}
				}
				if (mine) {
					fault.slug = model->slug;
					fault.version = model->version;
					name = model->name;
				}
			}
		}
		// The library it was in says more exactly.
		size_t space = f.tag.rfind(' ');
		if (space != std::string::npos) {
			fault.slug = f.tag.substr(0, space);
			fault.version = atoi(f.tag.c_str() + space + 1);
			if (name.empty())
				name = fault.slug;
		}
		LOGW("%s version %d: %s %s, at 0x%lx", fault.slug.c_str(), fault.version, fault.what.c_str(), fault.where.c_str(), (unsigned long) fault.offset);
		// The same thing again and again, while it is being looked at, once.
		bool again = !g_faults.empty() && g_faults.back().slug == fault.slug && g_faults.back().offset == fault.offset
			&& g_faults.back().what == fault.what && fault.time - g_faults.back().time < 5.0;
		if (again)
			continue;
		g_faults.push_back(fault);
		while (g_faults.size() > 20)
			g_faults.pop_front();
		if (!name.empty())
			showToast(name + " has stopped: an error in its code. The chat can look for it.");
	}
}


} // namespace rackdroid


/** At startup, before there is a patch; the render thread waits meanwhile. */
extern "C" JNIEXPORT jstring JNICALL
Java_org_rackdroid_MadeModules_nativeMadeAtStart(JNIEnv* env, jobject, jbyteArray request) {
	jsize n = env->GetArrayLength(request);
	std::string in((size_t) n, '\0');
	env->GetByteArrayRegion(request, 0, n, (jbyte*) &in[0]);
	json_error_t error;
	json_t* req = json_loads(in.c_str(), 0, &error);
	std::string out;
	try {
		rackdroid::opLoad(req, false);
	}
	catch (std::exception& e) {
		out = e.what();
	}
	json_decref(req);
	return env->NewStringUTF(out.c_str());
}
