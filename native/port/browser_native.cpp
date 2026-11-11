/* Native Android module browser.
 *
 * Rack's "Add module" browser (third_party/Rack/src/app/Browser.cpp) is a
 * custom widget::OpaqueWidget, not a ui::Menu -- the menu_native.cpp bridge
 * can't capture it. This bridge takes a different, simpler approach: rather
 * than serializing live widget state every frame, it hides the canvas
 * browser the moment it opens and hands Java a one-shot JSON snapshot of
 * every model (brand/name/tags), built once and cached forever
 * (the plugin list never changes after startup). Java does its own
 * search/filtering client-side against that snapshot -- no round trip per
 * keystroke.
 *
 * Actions flow back marshalled onto the render thread (Rack widgets aren't
 * thread-safe) via a pending-slot-plus-mutex, processed in
 * processNativeBrowser() next frame -- same shape as menu_native.cpp's
 * atomic pending flags, just carrying a string payload:
 *  - chooseAt(slug, x, y): instantiate + place the module (mirrors
 *    Browser.cpp's chooseModel(), minus the mouse-drag choreography).
 *  - unloadPlugin(slug): drop a plugin from the registry on pack uninstall.
 */
#include <atomic>
#include <mutex>
#include <string>

#include <jansson.h>

#include <context.hpp>
#include <common.hpp>
#include <app/ModuleWidget.hpp>
#include <app/Scene.hpp>
#include <engine/Engine.hpp>
#include <history.hpp>
#include <plugin.hpp>
#include <plugin/Model.hpp>
#include <plugin/Plugin.hpp>
#include <settings.hpp>
#include <string.hpp>
#include <system.hpp>
#include <tag.hpp>

#include <jni.h>
#include <android/log.h>

#include "browser_native.hpp"
#include "label_overlay.hpp"
#include "gamepad_nav.hpp"
#include "jni_bridge.hpp"

#define LOGI(...) do { __android_log_print(ANDROID_LOG_INFO, "rackdroid.browser", __VA_ARGS__); INFO(__VA_ARGS__); } while (0)
#define LOGE(...) do { __android_log_print(ANDROID_LOG_ERROR, "rackdroid.browser", __VA_ARGS__); WARN(__VA_ARGS__); } while (0)

using namespace rack;


namespace rackdroid {


struct BrowserBridge {
	bool wasVisible = false;
	bool modelsBuilt = false;
	std::string modelsJson = "[]";
	// Java reads the snapshot on the UI thread while the render thread rebuilds
	// it after a pack install. A mutex protects the string itself; the generation
	// lets Java wait for the requested rebuild rather than guessing a delay.
	std::mutex modelsMutex;
	std::atomic<uint64_t> modelsGeneration{0};

	std::mutex actionMutex;
	std::string pendingChoose;   // "pluginSlug/modelSlug", empty = none
	// Screen-pixel drop position for the choose (module palette drag&drop);
	// negative = no position, place at the rack's current mouse pos. Reachable
	// when a tile is dropped past the left/top edge of the screen.
	float pendingChooseX = -1.f;
	float pendingChooseY = -1.f;
	// Slug of a plugin to drop from the registry live (module pack uninstall),
	// so its models vanish from the browser/palette without a restart.
	std::string pendingUnload;
	// Ask the render thread to build the models JSON ahead of time (the
	// palette needs it before the canvas browser has ever been opened).
	std::atomic<bool> buildRequested{false};
};

static BrowserBridge g;


/** "pluginSlug/modelSlug" -> Model*, or NULL. Plugin/model slugs never
 * contain '/' (Rack's own slugify rules), so a single split is unambiguous. */
static plugin::Model* findModel(const std::string& key) {
	size_t slash = key.find('/');
	if (slash == std::string::npos)
		return NULL;
	std::string pluginSlug = key.substr(0, slash);
	std::string modelSlug = key.substr(slash + 1);
	for (plugin::Plugin* p : plugin::plugins) {
		if (p->slug != pluginSlug)
			continue;
		for (plugin::Model* m : p->models) {
			if (m->slug == modelSlug)
				return m;
		}
	}
	return NULL;
}


/** True if scene->browser is still a live child of the scene. A patch
 * reload (e.g. File > New) can tear down and rebuild the rack/scene
 * contents out from under this per-frame poll; touching the cached
 * pointer without checking crashed with a use-after-free (SIGSEGV inside
 * Widget::setVisible, hit live during this session) -- same class of bug
 * menu_native.cpp's overlayAlive() already guards against for menus. */
static bool browserAlive(app::Scene* scene) {
	if (!scene->browser)
		return false;
	for (widget::Widget* c : scene->children) {
		if (c == scene->browser)
			return true;
	}
	return false;
}


static void buildModelsJson() {
	json_t* arr = json_array();
	for (plugin::Plugin* p : plugin::plugins) {
		for (plugin::Model* m : p->models) {
			json_t* o = json_object();
			json_object_set_new(o, "key", json_string((p->slug + "/" + m->slug).c_str()));
			json_object_set_new(o, "name", json_string(m->name.c_str()));
			json_object_set_new(o, "brand", json_string(p->brand.c_str()));
			json_object_set_new(o, "description", json_string(m->description.c_str()));
			json_object_set_new(o, "plugin", json_string(p->name.c_str()));
			json_object_set_new(o, "version", json_string(p->version.c_str()));
			json_object_set_new(o, "license", json_string(p->license.c_str()));
			json_t* tags = json_array();
			for (int tagId : m->tagIds)
				json_array_append_new(tags, json_string(tag::getTag(tagId).c_str()));
			json_object_set_new(o, "tags", tags);
			json_array_append_new(arr, o);
		}
	}
	size_t modelCount = json_array_size(arr);
	char* dump = json_dumps(arr, JSON_COMPACT);
	std::string snapshot = dump ? dump : "[]";
	size_t snapshotBytes = snapshot.size();
	free(dump);
	json_decref(arr);
	{
		std::lock_guard<std::mutex> lock(g.modelsMutex);
		g.modelsJson.swap(snapshot);
	}
	g.modelsBuilt = true;
	g.modelsGeneration.fetch_add(1, std::memory_order_release);
	LOGI("built model list json (%zu models, %zu bytes)", modelCount, snapshotBytes);
}


/** Mirrors Browser.cpp's chooseModel(), minus the mouse-drag choreography
 * (loadTemplate/history/addModuleAtMouse are all still real -- only the
 * "redirect the ongoing click to the new widget" trick is dropped, since a
 * tap-to-place tile has no ongoing drag to redirect). */
static void chooseModel(plugin::Model* model) {
	settings::ModuleInfo& mi = settings::moduleInfos[model->plugin->slug][model->slug];
	mi.added++;
	mi.lastAdded = system::getUnixTime();

	history::ComplexAction* h = new history::ComplexAction;
	h->name = string::translate("Browser.history.addModule");

	LOGI("creating module %s", model->getFullName().c_str());
	engine::Module* module = model->createModule();
	APP->engine->addModule(module);
	app::ModuleWidget* moduleWidget = model->createModuleWidget(module);

	APP->scene->rack->deselectAll();
	APP->scene->rack->updateModuleOldPositions();
	APP->scene->rack->addModuleAtMouse(moduleWidget);
	h->push(APP->scene->rack->getModuleDragAction());

	moduleWidget->loadTemplate();
	noteModuleAdded(module->id);
	gamepadNavFocusModule(module->id);

	history::ModuleAdd* ha = new history::ModuleAdd;
	ha->setModule(moduleWidget);
	h->push(ha);

	APP->history->push(h);
}


void processNativeBrowser() {
	app::Scene* scene = APP->scene;
	if (!scene || !browserAlive(scene)) {
		// Reset so a freshly (re)created browser -- after a patch reload
		// tears down and rebuilds the one we were tracking -- is detected
		// as a brand new open rather than compared against stale state.
		g.wasVisible = false;
	}
	else {
		bool visible = scene->browser->visible;
		if (visible && !g.wasVisible) {
			// Rack just opened its own browser (long-press empty rack
			// space, Enter key): suppress it and show ours instead.
			scene->browser->hide();
			if (!g.modelsBuilt)
				buildModelsJson();
			nativeBrowserShow();
		}
		g.wasVisible = visible;
	}

	// An explicit request FORCES a rebuild (not just the first-time lazy
	// build): installing a module pack at runtime adds to plugin::plugins
	// after modelsBuilt is already true, and the new modules must appear in
	// the browser/palette live -- so requestBuild is the authoritative "the
	// plugin set changed, rebuild now" signal. Callers gate it themselves
	// (startup once, palette reload after install), so this never spams.
	if (g.buildRequested.exchange(false))
		buildModelsJson();

	std::string choose;
	float chooseX = -1.f, chooseY = -1.f;
	std::string unload;
	{
		std::lock_guard<std::mutex> lock(g.actionMutex);
		choose.swap(g.pendingChoose);
		chooseX = g.pendingChooseX;
		chooseY = g.pendingChooseY;
		g.pendingChooseX = g.pendingChooseY = -1.f;
		unload.swap(g.pendingUnload);
	}

	if (!unload.empty()) {
		// Uninstall: drop the plugin from the registry so buildModelsJson no
		// longer lists it -- the palette loses its modules immediately. The
		// Plugin/Model objects are deliberately NOT freed and the .so stays
		// dlopen'd: any instances the user already placed hold a direct Model*
		// and must keep working until the app restarts (a real unload would
		// dangle them). A small, bounded leak in exchange for safety.
		for (auto it = plugin::plugins.begin(); it != plugin::plugins.end(); ++it) {
			if ((*it)->slug == unload) {
				LOGI("unloading plugin %s from registry (%zu models)",
					unload.c_str(), (*it)->models.size());
				plugin::plugins.erase(it);
				break;
			}
		}
		buildModelsJson();
	}

	if (!choose.empty()) {
		try {
			plugin::Model* model = findModel(choose);
			if (model) {
				if (chooseX >= 0.f && APP->window) {
					// Palette drop: move the virtual cursor to the drop
					// point first, so addModuleAtMouse lands there (same
					// px -> scene conversion as touch_input.cpp).
					float pr = APP->window->pixelRatio;
					APP->event->handleHover(
						math::Vec(chooseX, chooseY).div(pr), math::Vec());
				}
				chooseModel(model);
			}
			else {
				LOGE("choose: unknown model key %s", choose.c_str());
			}
		}
		catch (std::exception& e) {
			LOGE("choose failed: %s", e.what());
		}
	}
}


// ---- JNI callbacks from MainActivity (Java UI thread) ----

extern "C" JNIEXPORT jstring JNICALL
Java_org_rackdroid_MainActivity_nativeBrowserModelsJson(JNIEnv* env, jobject) {
	std::lock_guard<std::mutex> lock(g.modelsMutex);
	return env->NewStringUTF(g.modelsJson.c_str());
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_rackdroid_MainActivity_nativeBrowserModelsGeneration(JNIEnv*, jobject) {
	return (jlong) g.modelsGeneration.load(std::memory_order_acquire);
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeBrowserChooseAt(JNIEnv* env, jobject, jstring key, jfloat x, jfloat y) {
	const char* chars = env->GetStringUTFChars(key, NULL);
	if (chars) {
		std::lock_guard<std::mutex> lock(g.actionMutex);
		g.pendingChoose = chars;
		g.pendingChooseX = x;
		g.pendingChooseY = y;
	}
	env->ReleaseStringUTFChars(key, chars);
}

extern "C" JNIEXPORT jlong JNICALL
Java_org_rackdroid_MainActivity_nativeBrowserRequestBuild(JNIEnv*, jobject) {
	// Return the generation the caller must observe advancing. Setting the flag
	// afterwards is safe: a concurrent render pass can only produce a newer
	// generation, which also satisfies the caller.
	jlong before = (jlong) g.modelsGeneration.load(std::memory_order_acquire);
	g.buildRequested.store(true);
	return before;
}

extern "C" JNIEXPORT void JNICALL
Java_org_rackdroid_MainActivity_nativeBrowserUnloadPlugin(JNIEnv* env, jobject, jstring slug) {
	const char* chars = env->GetStringUTFChars(slug, NULL);
	{
		std::lock_guard<std::mutex> lock(g.actionMutex);
		g.pendingUnload = chars ? chars : "";
	}
	env->ReleaseStringUTFChars(slug, chars);
}


} // namespace rackdroid
