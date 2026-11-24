package org.rackdroid

import android.content.SharedPreferences
import org.json.JSONObject

/** What the chat is answered by, and what it is typed with. The names of models
 * and levels are Claude Code's own: an alias for the newest model of a kind, and
 * its levels of effort. */
class ChatSettings(private val prefs: SharedPreferences) {
	class Choice(val id: String, val name: String, val note: String)

	companion object {
		val MODELS = listOf(
			Choice("fable", "Fable", "The most capable, and uses the plan up fastest"),
			Choice("opus", "Opus", "Careful: measures and puts right what it made"),
			Choice("sonnet", "Sonnet", "A little quicker"),
			Choice("haiku", "Haiku", "The smallest"))
		val EFFORTS = listOf(
			Choice("low", "low", "Quick, and enough for most changes"),
			Choice("medium", "medium", ""),
			Choice("high", "high", "For large patches"),
			Choice("xhigh", "xhigh", ""),
			Choice("max", "max", "Can take minutes"))
	}

	val model: String get() = known(MODELS, prefs.getString("chatModel", null), "opus")
	val effort: String get() = known(EFFORTS, prefs.getString("chatEffort", null), "low")
	val modelName: String get() = MODELS.first { it.id == model }.name

	/** Type with the D-pad (PadKeyboard) and not with Android's keyboard. */
	var padKeyboard: Boolean
		get() = prefs.getBoolean("chatPadKeyboard", false)
		set(on) = prefs.edit().putBoolean("chatPadKeyboard", on).apply()

	private fun known(all: List<Choice>, id: String?, otherwise: String) =
		if (all.any { it.id == id }) id!! else otherwise

	fun set(model: String, effort: String) =
		prefs.edit().putString("chatModel", model).putString("chatEffort", effort).apply()

	fun json(): JSONObject = JSONObject().put("model", model).put("effort", effort)
}
