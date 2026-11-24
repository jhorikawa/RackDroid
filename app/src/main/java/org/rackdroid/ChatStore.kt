package org.rackdroid

import android.content.SharedPreferences
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * The chats so far, one file each: what was said, and which conversation of
 * Claude Code's it is, so that it can be gone on with (the conversation itself is
 * Claude Code's to keep). The last one is where the panel goes on after the app
 * has been closed.
 *
 *   files/user/chats/<when it was begun>.json   {"session", "at", "lines": [{"who", "text", "work"}]}
 *
 * UI thread.
 */
class ChatStore(private val dir: File, private val prefs: SharedPreferences) {
	class Line(val who: String, val text: String, val work: String)

	class Chat(val id: String, var session: String, var at: Long, val lines: ArrayList<Line>) {
		/** What was asked first. */
		val title: String get() = lines.firstOrNull { it.who == USER }?.text?.trim()?.replace(Regex("\\s+"), " ") ?: ""
	}

	companion object {
		const val USER = "user"
		const val CLAUDE = "claude"
		private const val KEY = "chatCurrent"
		private const val MAX_LINES = 600
		private const val MAX_CHATS = 200
	}

	var current: Chat = prefs.getString(KEY, null)?.let { read(it) } ?: fresh()
		private set

	private fun fresh() = Chat(SimpleDateFormat("yyyyMMdd-HHmmss-SSS", Locale.US).format(Date()), "", System.currentTimeMillis(), ArrayList())

	private fun file(id: String) = File(dir, "$id.json")

	private fun read(id: String): Chat? = runCatching {
		val o = JSONObject(file(id).readText())
		val a = o.getJSONArray("lines")
		Chat(id, o.optString("session"), o.optLong("at"), ArrayList((0 until a.length()).map {
			val l = a.getJSONObject(it)
			Line(l.getString("who"), l.getString("text"), l.optString("work"))
		}))
	}.getOrNull()

	private fun write(c: Chat) {
		runCatching {
			dir.mkdirs()
			val lines = JSONArray()
			c.lines.forEach { lines.put(JSONObject().put("who", it.who).put("text", it.text).put("work", it.work)) }
			val tmp = File(dir, "${c.id}.tmp")
			tmp.writeText(JSONObject().put("session", c.session).put("at", c.at).put("lines", lines).toString())
			tmp.renameTo(file(c.id))
			prefs.edit().putString(KEY, c.id).apply()
		}.onFailure { android.util.Log.w("rackdroid.chat", "cannot keep the chat", it) }
	}

	fun add(who: String, text: String, work: String) {
		val c = current
		c.lines.add(Line(who, text, work))
		while (c.lines.size > MAX_LINES)
			c.lines.removeAt(0)
		c.at = System.currentTimeMillis()
		write(c)
	}

	/** Which conversation of Claude Code's the current chat is. */
	fun setSession(id: String) {
		if (current.session == id)
			return
		current.session = id
		if (current.lines.isNotEmpty())
			write(current)
	}

	fun startNew() {
		if (current.lines.isNotEmpty())
			current = fresh()
		current.session = ""
		prefs.edit().remove(KEY).apply()
	}

	fun open(id: String): Boolean {
		current = read(id) ?: return false
		prefs.edit().putString(KEY, id).apply()
		return true
	}

	/** The newest first. */
	fun list(): List<Chat> {
		val all = (dir.listFiles { f -> f.name.endsWith(".json") } ?: emptyArray())
			.mapNotNull { read(it.name.removeSuffix(".json")) }.filter { it.lines.isNotEmpty() }.sortedByDescending { it.at }
		all.drop(MAX_CHATS).forEach { file(it.id).delete() }
		return all.take(MAX_CHATS)
	}

	fun delete(id: String) {
		file(id).delete()
		if (current.id == id) {
			current = fresh()
			prefs.edit().remove(KEY).apply()
		}
	}
}
