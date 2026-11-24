package org.rackdroid

import android.app.Activity
import android.util.Base64
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.zip.ZipInputStream

/**
 * Modules made on this device from the chat: where they are kept, which version
 * of each runs, and what is done about one that brings the app down.
 *
 *   files/user/made/<id>/state.json    {"current": 3, "off": false, "why": "", "place": 0}
 *   files/user/made/<id>/3/            made.json, module.cpp, libnova_made_<id>_3_<time>.so
 *
 * Each version is a library of its own name, so a new one is loaded beside the
 * old one while the app runs (made_modules.cpp exchanges them in the patch).
 *
 * The code may be wrong. What can be caught is caught as it runs
 * (module_guard.cpp); what cannot leaves a mark, and the next start stands a
 * placeholder in its place, so that the patch opens, whole, with that module
 * silent. The sources are also put where a computer can read them
 * (Android/data/.../files/MadeModules), for desktop VCV Rack.
 */
object MadeModules {
	const val PLUGIN = "NovaMade"
	private const val KEEP = 6
	private val ID = Regex("[A-Z][A-Za-z0-9]{1,23}")
	const val MAX_CODE = 200_000
	const val MAX_LIBRARY = 8 * 1024 * 1024

	/** What the last start had to turn off, to be told to the user. */
	private val turnedOff = ArrayList<String>()
	/** Those that wait to be made again for this version of the app. */
	private val waiting = ArrayList<String>()

	private fun root(activity: Activity) = File(activity.filesDir, "user/made")
	private fun crashMark(activity: Activity) = File(root(activity), ".crashed")
	private fun loadMark(activity: Activity) = File(root(activity), ".loading")
	private fun version(activity: Activity, id: String, n: Int) = File(root(activity), "$id/$n")

	/** place: where in its library the app had crashed, or 0. */
	private class State(var current: Int, var off: Boolean, var why: String, var place: Long = 0L)

	private fun state(activity: Activity, id: String): State? = runCatching {
		val o = JSONObject(File(root(activity), "$id/state.json").readText())
		State(o.getInt("current"), o.optBoolean("off"), o.optString("why"), o.optLong("place"))
	}.getOrNull()

	private fun save(activity: Activity, id: String, s: State) {
		val f = File(root(activity), "$id/state.json")
		val tmp = File(f.path + ".tmp")
		tmp.writeText(JSONObject().put("current", s.current).put("off", s.off).put("why", s.why)
			.put("place", s.place).toString())
		tmp.renameTo(f)
	}

	private val LIBRARY = Regex("libnova_made_[A-Za-z0-9_]+\\.so")

	/** A name no library has had before: what was loaded under one stays loaded
	 * under it, even when its file is another by then. */
	private fun newLibrary(id: String, n: Int) = "libnova_made_${id}_${n}_${System.currentTimeMillis()}.so"

	private fun library(activity: Activity, id: String, n: Int): File? =
		meta(activity, id, n)?.optString("library")?.takeIf { LIBRARY.matches(it) }?.let { File(version(activity, id, n), it) }

	private fun meta(activity: Activity, id: String, n: Int): JSONObject? =
		runCatching { JSONObject(File(version(activity, id, n), "made.json").readText()) }.getOrNull()

	private fun versions(activity: Activity, id: String): List<Int> =
		File(root(activity), id).listFiles { f -> f.isDirectory }?.mapNotNull { it.name.toIntOrNull() }?.sorted() ?: emptyList()

	fun ids(activity: Activity): List<String> =
		root(activity).listFiles { f -> f.isDirectory && ID.matches(f.name) }?.map { it.name }?.sorted() ?: emptyList()

	/** The headers this app's engine was built with. */
	private var abi: String? = null
	private fun abi(activity: Activity): String = abi ?: runCatching {
		activity.assets.open("module-kit.abi").bufferedReader().use { it.readText().trim() }
	}.getOrDefault("none").also { abi = it }

	/* ---- The start ---- */

	/** Every one of them, or its placeholder. On the thread that loads the packs,
	 * before the patch is opened. */
	fun loadAll(activity: Activity) {
		val ids = ids(activity)
		if (ids.isEmpty())
			return
		blame(activity, loadMark(activity), "RackDroid closed while it was being loaded")
		blame(activity, crashMark(activity), "RackDroid crashed in its code")
		for (id in ids) {
			try {
				val s = state(activity, id) ?: continue
				val why = load(activity, id, s)
				thumbnail(activity, id, s.current, false)
				val problem = nativeMadeAtStart(request(activity, id, s, why).toString().toByteArray())
				if (problem.isNotEmpty())
					jlog("$id: $problem")
			} catch (t: Throwable) {
				jlog("$id cannot be loaded: $t")
			}
		}
		runCatching { loadMark(activity).delete() }
	}

	private fun thumbnail(activity: Activity, id: String, n: Int, anew: Boolean) {
		val key = "$PLUGIN/$id"
		if (!anew && MadeThumbnail.file(activity.filesDir, key).exists())
			return
		runCatching { meta(activity, id, n)?.let { MadeThumbnail.write(activity.filesDir, key, id, it) } }
			.onFailure { jlog("no picture of $id: $it") }
		if (anew)
			activity.runOnUiThread { ThumbnailCache.removePlugin(PLUGIN) }
	}

	/** The app could not be started twice in a row: without them, then. */
	fun recover(activity: Activity) {
		for (id in ids(activity)) {
			val s = state(activity, id) ?: continue
			if (!s.off)
				turnOff(activity, id, s, "RackDroid could not be started with it")
		}
	}

	private fun turnOff(activity: Activity, id: String, s: State, why: String) {
		s.off = true
		s.why = why
		save(activity, id, s)
		turnedOff.add(meta(activity, id, s.current)?.optString("name") ?: id)
		jlog("$id turned off: $why")
	}

	private fun blame(activity: Activity, mark: File, why: String) {
		val lines = runCatching { mark.readLines() }.getOrNull() ?: return
		mark.delete()
		// "Folder 3 0x1e6a0", the place only with the first.
		for (words in lines.map { it.split(' ') }.filter { ID.matches(it[0]) }.distinctBy { it[0] }) {
			val s = state(activity, words[0]) ?: continue
			if (s.off)
				continue
			if (words.size > 2 && words[1] == s.current.toString())
				s.place = runCatching { java.lang.Long.decode(words[2]) }.getOrDefault(0L)
			turnOff(activity, words[0], s, why)
		}
	}

	/** For the user, once: what is off since the start. */
	fun takeNotice(): String? {
		// A toast has two lines.
		fun those(names: List<String>, one: String, more: String) =
			if (names.size == 1) "${names[0]} $one" else "${names.size} made modules $more"
		val text = when {
			turnedOff.isNotEmpty() -> those(turnedOff, "is", "are") + " stopped: RackDroid had crashed in " +
				(if (turnedOff.size == 1) "it" else "one") + ". Ask the chat, or START > Made modules."
			waiting.isNotEmpty() -> those(waiting, "waits", "wait") + " to be made again for this RackDroid: open the chat."
			else -> null
		}
		turnedOff.clear()
		waiting.clear()
		return text
	}

	/** Brings the library into the process. Returns why not, or "". */
	private fun load(activity: Activity, id: String, s: State): String {
		if (s.off)
			return s.why.ifEmpty { "it is turned off" }
		val m = meta(activity, id, s.current) ?: return "its files are missing"
		if (m.optString("abi") != abi(activity)) {
			waiting.add(m.optString("name", id))
			return "it was made for another version of RackDroid, and is made again when the chat is opened"
		}
		val so = library(activity, id, s.current)
		if (so == null || !so.isFile)
			return "its files are missing"
		val mark = loadMark(activity)
		return try {
			mark.writeText("$id ${s.current}\n")
			so.setReadOnly()
			System.load(so.absolutePath)
			""
		} catch (t: Throwable) {
			"it cannot be loaded (${t.message})"
		} finally {
			mark.delete()
		}
	}

	private fun request(activity: Activity, id: String, s: State, why: String) = JSONObject()
		.put("op", "made_load").put("slug", id).put("version", s.current)
		.put("meta", meta(activity, id, s.current) ?: JSONObject())
		.put("dir", version(activity, id, s.current).absolutePath)
		.put("soname", if (why.isEmpty()) library(activity, id, s.current)?.name ?: "" else "")
		.put("why", why)
		.put("marker", crashMark(activity).absolutePath)

	/* ---- While it runs (not on the UI thread) ---- */

	private class Refused(text: String) : Exception(text)

	private fun answer(text: String) = JSONObject().put("ok", true).put("text", text).toString()

	/** One of the operations "made_..." of the chat (PatchServer). `native` passes
	 * a request on to the render thread and waits. */
	fun operate(activity: Activity, o: JSONObject, native: (JSONObject) -> JSONObject, rebuildList: () -> Unit): String = try {
		synchronized(this) {
			when (o.getString("op")) {
				"made_kit" -> answer(JSONObject().put("apk", activity.applicationInfo.sourceDir).put("abi", abi(activity)).toString())
				"made_list" -> answer(list(activity, native))
				"made_source" -> answer(source(activity, o))
				"made_install" -> answer(install(activity, o, native)).also { rebuildList() }
				"made_revert" -> answer(revert(activity, o, native)).also { rebuildList() }
				"made_switch" -> answer(switch(activity, o, native)).also { rebuildList() }
				"made_remove" -> answer(remove(activity, o, native)).also { rebuildList() }
				"made_faults" -> native(o).toString()
				else -> throw Refused("There is no operation \"${o.getString("op")}\".")
			}
		}
	} catch (t: Throwable) {
		JSONObject().put("ok", false).put("error", (t as? Refused)?.message ?: "That did not work: $t").toString()
	}

	private fun done(o: JSONObject): String {
		if (!o.optBoolean("ok"))
			throw Refused(o.optString("error"))
		return o.optString("text")
	}

	private fun idOf(activity: Activity, o: JSONObject): String {
		val id = o.optString("slug").substringAfter('/')
		if (!ID.matches(id) || state(activity, id) == null)
			throw Refused("There is no made module \"$id\".")
		return id
	}

	/** As JSON: the bridge puts it into words. */
	private fun list(activity: Activity, native: (JSONObject) -> JSONObject): String {
		val status = runCatching { JSONObject(done(native(JSONObject().put("op", "made_status")))) }.getOrDefault(JSONObject())
		val out = JSONArray()
		for (id in ids(activity)) {
			val s = state(activity, id) ?: continue
			val m = meta(activity, id, s.current) ?: JSONObject()
			val now = status.optJSONObject(id) ?: JSONObject()
			out.put(JSONObject().put("id", id).put("key", "$PLUGIN/$id").put("name", m.optString("name"))
				.put("description", m.optString("description")).put("version", s.current)
				.put("versions", JSONArray(versions(activity, id))).put("off", s.off).put("why", s.why).put("place", s.place)
				.put("stale", m.optString("abi") != abi(activity))
				.put("in_patch", now.optInt("in_patch")).put("stopped", now.optString("stopped")))
		}
		return out.toString()
	}

	/** For the menu: how they stand, without asking the engine. */
	class Row(val id: String, val name: String, val version: Int, val versions: List<Int>, val off: Boolean, val why: String)

	fun rows(activity: Activity): List<Row> = ids(activity).mapNotNull { id ->
		val s = state(activity, id) ?: return@mapNotNull null
		val m = meta(activity, id, s.current)
		val stale = m?.optString("abi") != abi(activity)
		Row(id, m?.optString("name") ?: id, s.current, versions(activity, id), s.off,
			if (s.off) s.why else if (stale) "made for another version of RackDroid: open the chat" else "")
	}

	private fun source(activity: Activity, o: JSONObject): String {
		val id = idOf(activity, o)
		val n = if (o.has("version")) o.getInt("version") else state(activity, id)!!.current
		val m = meta(activity, id, n) ?: throw Refused("There is no version $n of $id.")
		return JSONObject().put("id", id).put("version", n).put("meta", m)
			.put("code", File(version(activity, id, n), "module.cpp").readText()).toString()
	}

	private fun install(activity: Activity, o: JSONObject, native: (JSONObject) -> JSONObject): String {
		val id = o.getString("slug")
		if (!ID.matches(id))
			throw Refused("An id is a capital letter and up to 23 letters and digits.")
		val meta = o.getJSONObject("meta")
		val code = o.getString("code")
		val library = Base64.decode(o.getString("library"), Base64.DEFAULT)
		if (code.length > MAX_CODE || library.size > MAX_LIBRARY)
			throw Refused("That is too large.")
		if (meta.optString("abi") != abi(activity))
			throw Refused("It was made for another version of RackDroid.")
		val before = state(activity, id)
		val n = (versions(activity, id).lastOrNull() ?: 0) + 1
		val dir = version(activity, id, n)
		dir.mkdirs()
		val so = File(dir, newLibrary(id, n))
		File(dir, "made.json").writeText(meta.put("version", n).put("library", so.name).toString())
		File(dir, "module.cpp").writeText(code)
		so.writeBytes(library)
		val s = State(n, false, "")
		val why = load(activity, id, s)
		if (why.isNotEmpty()) {
			dir.deleteRecursively()
			throw Refused("It is compiled, but $why.")
		}
		val text = try {
			done(native(request(activity, id, s, "")))
		} catch (t: Refused) {
			if (before == null)
				File(root(activity), id).deleteRecursively()
			throw t
		}
		save(activity, id, s)
		for (old in versions(activity, id).dropLast(KEEP).filter { it != n })
			version(activity, id, old).deleteRecursively()
		mirror(activity, id, n)
		thumbnail(activity, id, n, true)
		jlog("$id version $n installed")
		return "$PLUGIN/$id is version $n now. $text".trim()
	}

	private fun change(activity: Activity, id: String, s: State, native: (JSONObject) -> JSONObject): String {
		val why = load(activity, id, s)
		if (why.isNotEmpty() && !s.off)
			throw Refused("$id version ${s.current}: $why.")
		val text = done(native(request(activity, id, s, why)))
		save(activity, id, s)
		return text
	}

	private fun revert(activity: Activity, o: JSONObject, native: (JSONObject) -> JSONObject): String {
		val id = idOf(activity, o)
		val n = o.getInt("version")
		if (n !in versions(activity, id))
			throw Refused("There is no version $n of $id, only ${versions(activity, id).joinToString(", ")}.")
		val text = change(activity, id, State(n, false, ""), native)
		mirror(activity, id, n)
		thumbnail(activity, id, n, true)
		return "$PLUGIN/$id is version $n again. $text".trim()
	}

	private fun switch(activity: Activity, o: JSONObject, native: (JSONObject) -> JSONObject): String {
		val id = idOf(activity, o)
		val s = state(activity, id)!!
		val on = o.getBoolean("on")
		val text = change(activity, id, State(s.current, !on, if (on) "" else "it was turned off"), native)
		return (if (on) "$id runs again. " else "$id is turned off. ") + text
	}

	private fun remove(activity: Activity, o: JSONObject, native: (JSONObject) -> JSONObject): String {
		val id = idOf(activity, o)
		done(native(JSONObject().put("op", "made_drop").put("slug", id)))
		// The sources are small, and cannot be had again.
		val bin = File(root(activity), ".removed/$id-${System.currentTimeMillis()}")
		bin.mkdirs()
		for (n in versions(activity, id)) {
			runCatching { File(version(activity, id, n), "module.cpp").copyTo(File(bin, "$n.cpp")) }
			runCatching { File(version(activity, id, n), "made.json").copyTo(File(bin, "$n.json")) }
		}
		File(root(activity), id).deleteRecursively()
		MadeThumbnail.file(activity.filesDir, "$PLUGIN/$id").delete()
		val out = File(activity.getExternalFilesDir(null), "MadeModules")
		File(out, "$id.cpp").delete()
		File(out, "$id.json").delete()
		return "$PLUGIN/$id is removed. A patch that has it can no longer make it."
	}

	/** The source, and what it is written against, for a computer to fetch
	 * (nova/tools/made-to-mac.sh). */
	private fun mirror(activity: Activity, id: String, n: Int) {
		try {
			val out = File(activity.getExternalFilesDir(null), "MadeModules").apply { mkdirs() }
			val dir = version(activity, id, n)
			File(dir, "module.cpp").copyTo(File(out, "$id.cpp"), true)
			File(dir, "made.json").copyTo(File(out, "$id.json"), true)
			ZipInputStream(activity.assets.open("module-kit.zip")).use { zip ->
				var e = zip.nextEntry
				while (e != null) {
					if (e.name == "nova_kit.hpp" || e.name == "nova_kit_impl.hpp")
						File(out, e.name).outputStream().use { zip.copyTo(it) }
					e = zip.nextEntry
				}
			}
		} catch (t: Throwable) {
			jlog("the source of $id cannot be put out: $t")
		}
	}

	private fun jlog(text: String) {
		android.util.Log.i("rackdroid.made", text)
	}

	private external fun nativeMadeAtStart(request: ByteArray): String
}
