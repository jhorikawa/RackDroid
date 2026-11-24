package org.rackdroid

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.drawable.Drawable
import android.util.TypedValue
import android.view.KeyEvent
import android.view.View
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import androidx.core.graphics.PathParser
import org.json.JSONArray

/**
 * Menus for a game controller (gamepad_nav.cpp): START opens the main menu, X
 * goes straight to adding a module.
 *
 * The toolbar and the module palette are built for a finger and deliberately
 * never take key focus, so a controller cannot reach them. Rather than rework
 * them, these plain list dialogs lead to the same places: the menu row's
 * entries open the same Rack menus, and "Tools" presses the toolbar's own
 * buttons, so nothing here has a behaviour of its own to keep in step.
 *
 * Every row carries a picture: the toolbar's own icon where the row is a
 * toolbar button, the module's panel where it is a module, and a small drawn
 * symbol otherwise. On a 4.5" screen a column of words takes reading; a
 * waveform next to "VCO" does not.
 *
 * A dialog is its own window, and Android moves a list's selection with the
 * D-pad by itself (a HAT axis included). A, B, L1/R1 and START are handled
 * here so they do not depend on a device's key fallbacks.
 */
class GamepadMenu(
	private val activity: Activity,
	private val getModelsJson: () -> String,
	private val requestBuild: () -> Unit,
	private val chooseAt: (String, Float, Float) -> Unit,
	/** Opens one of the Rack menus: the MenuBar's child index (File 0 .. Help 5). */
	private val openRackMenu: (Int) -> Unit,
	/** 0 undo, 1 redo. */
	private val history: (Int) -> Unit,
	/** A command for gamepad_nav.cpp, run on the render thread. */
	private val command: (Int) -> Unit,
	/** The toolbar's buttons, in toolbar order. */
	private val tools: () -> List<View>,
	/** Shows or hides the touch pad (XyPad.kt). */
	private val toggleXyPad: () -> Unit,
	private val paused: () -> Boolean,
	private val togglePause: () -> Unit,
	/** The moving arrows on the cables (signal_flow.cpp). */
	private val signalFlow: () -> Boolean,
	private val setSignalFlow: (Boolean) -> Unit,
	/** The key of the focused module's model, or "" (gamepad_nav.cpp). */
	private val focusedModel: () -> String,
	private val openChat: () -> Unit,
) {
	companion object {
		const val MAIN = 0
		const val ADD_MODULE = 1

		/** The category of Mutable Instruments ports. */
		private const val MUTABLE = "MUTABLE"

		/** Matches GamepadCommand in gamepad_nav.cpp. */
		const val CMD_MODULE_MENU = 1
		const val CMD_MOVE_MODULE = 2
		const val CMD_DELETE_MODULE = 3

		// Icons as SVG path data on a 24 x 24 grid. The filled ones are
		// Material Design icons (Apache 2.0), the family the toolbar's own
		// icons come from; the stroked ones are drawn for this menu.
		private const val FILL_MODULE_MENU = "M3,17v2h6v-2H3zM3,5v2h10V5H3zM13,21v-2h8v-2h-8v-2h-2v6h2zM7,9v2H3v2h4v2h2V9H7zM21,13v-2H11v2h10zM15,9h2V7h4V5h-4V3h-2v6z"
		private const val FILL_FILE = "M10,4H4c-1.1,0 -1.99,0.9 -1.99,2L2,18c0,1.1 0.9,2 2,2h16c1.1,0 2,-0.9 2,-2V8c0,-1.1 -0.9,-2 -2,-2h-8l-2,-2z"
		private const val FILL_EDIT = "M3,17.25V21h3.75L17.81,9.94l-3.75,-3.75L3,17.25zM20.71,7.04c0.39,-0.39 0.39,-1.02 0,-1.41l-2.34,-2.34c-0.39,-0.39 -1.02,-0.39 -1.41,0l-1.83,1.83 3.75,3.75 1.83,-1.83z"
		private const val FILL_VIEW = "M12,4.5C7,4.5 2.73,7.61 1,12c1.73,4.39 6,7.5 11,7.5s9.27,-3.11 11,-7.5c-1.73,-4.39 -6,-7.5 -11,-7.5zM12,17c-2.76,0 -5,-2.24 -5,-5s2.24,-5 5,-5 5,2.24 5,5 -2.24,5 -5,5zM12,9c-1.66,0 -3,1.34 -3,3s1.34,3 3,3 3,-1.34 3,-3 -1.34,-3 -3,-3z"
		private const val FILL_ENGINE = "M15,9H9v6h6V9zM13,13h-2v-2h2v2zM21,11V9h-2V7c0,-1.1 -0.9,-2 -2,-2h-2V3h-2v2h-2V3H9v2H7c-1.1,0 -2,0.9 -2,2v2H3v2h2v2H3v2h2v2c0,1.1 0.9,2 2,2h2v2h2v-2h2v2h2v-2h2c1.1,0 2,-0.9 2,-2v-2h2v-2h-2v-2h2zM17,17H7V7h10v10z"
		private const val FILL_HELP = "M12,2C6.48,2 2,6.48 2,12s4.48,10 10,10 10,-4.48 10,-10S17.52,2 12,2zM13,19h-2v-2h2v2zM15.07,11.25l-0.9,0.92C13.45,12.9 13,13.5 13,15h-2v-0.5c0,-1.1 0.45,-2.1 1.17,-2.83l1.24,-1.26c0.37,-0.36 0.59,-0.86 0.59,-1.41 0,-1.1 -0.9,-2 -2,-2s-2,0.9 -2,2H8c0,-2.21 1.79,-4 4,-4s4,1.79 4,4c0,0.88 -0.36,1.68 -0.93,2.25z"
		private const val FILL_MOVE = "M10,9h4V6h3l-5,-5 -5,5h3v3zM9,10H6V7l-5,5 5,5v-3h3v-4zM23,12l-5,-5v3h-3v4h3v3l5,-5zM14,15h-4v3H7l5,5 5,-5h-3v-3z"
		private const val FILL_TOOLS = "M22.7,19l-9.1,-9.1c0.9,-2.3 0.4,-5 -1.5,-6.9 -2,-2 -5,-2.4 -7.4,-1.3L9,6 6,9 1.6,4.7C0.4,7.1 0.9,10.1 2.9,12.1c1.9,1.9 4.6,2.4 6.9,1.5l9.1,9.1c0.4,0.4 1,0.4 1.4,0l2.3,-2.3c0.5,-0.4 0.5,-1.1 0.1,-1.4z"

		private const val LINE_VCO = "M2,12 C3.3,4 5.7,4 7,12 C8.3,20 10.7,20 12,12 C13.3,4 15.7,4 17,12 C18.3,20 20.7,20 22,12"
		private const val LINE_LFO = "M2,12 C5,2 9,2 12,12 C15,22 19,22 22,12"
		private const val LINE_VCF = "M2,10 H9 C12,10 12.5,5 14.5,5 C17,5 18,12 21,20"
		private const val LINE_VCA = "M5,4 L20,12 L5,20 Z"
		private const val LINE_ENV = "M2,20 L7,4 L11,12 H16 L22,20"
		private const val LINE_SEQ = "M2,18 H6 V10 H10 V15 H14 V5 H18 V12 H22"
		private const val LINE_DRUM = "M4,8 A8,3 0 1 0 20,8 A8,3 0 1 0 4,8 M4,8 V16 A8,3 0 0 0 20,16 V8"
		private const val LINE_MIX = "M6,3 V21 M12,3 V21 M18,3 V21 M3.5,9 H8.5 M9.5,16 H14.5 M15.5,7 H20.5"
		private const val LINE_FX = "M12,2 L14,10 L22,12 L14,14 L12,22 L10,14 L2,12 L10,10 Z"
		private const val LINE_NOISE = "M2,12 L4,6 L6,17 L8,9 L10,19 L12,4 L14,15 L16,8 L18,18 L20,10 L22,13"
		private const val LINE_QNT = "M2,20 H6 V15 H11 V10 H16 V5 H22"
		private const val LINE_MIDI = "M12,3 A9,9 0 1 0 12.01,3 M7,13 h0.01 M17,13 h0.01 M8.6,8.6 h0.01 M15.4,8.6 h0.01 M12,7 h0.01"
		private const val LINE_XY = "M4,4 H20 V20 H4 Z M4,14 H20 M10,4 V20 M10,14 h0.01"
		private const val LINE_VISUAL = "M2,4 H22 V18 H2 Z M4,12 C6,5 8,5 10,11 C12,17 14,17 16,11 C17.2,7.5 18.5,7.5 20,10 M8,22 H16"
		private const val LINE_SEARCH = "M4,10 A6,6 0 1 0 16,10 A6,6 0 1 0 4,10 M14.5,14.5 L21,21"
		private const val LINE_TAG = "M3,3 H11 L21,13 L13,21 L3,11 Z M7.5,7.5 h0.01"
		private const val LINE_PLUS = "M12,4 V20 M4,12 H20"
		private const val LINE_BOX = "M4,4 H20 V20 H4 Z"
		private const val LINE_CHECKED = "M4,4 H20 V20 H4 Z M7.5,12 L11,15.5 L17,8.5"
		private const val LINE_FLOW = "M3,6 L9,12 L3,18 M9,6 L15,12 L9,18 M15,6 L21,12 L15,18"
		private const val LINE_CHAT = "M3,4 H21 V16 H11 L6,21 V16 H3 Z M8,10 h0.01 M12,10 h0.01 M16,10 h0.01"
		private const val LINE_MISC = "M5,12 h0.01 M12,12 h0.01 M19,12 h0.01"
		private const val LINE_MUTABLE = "M4,19 V5 L12,14 L20,5 V19"
	}

	/** @param origin the Mutable Instruments module this one is a port of
	 * ("Plaits"), or null (MutableOrigin). */
	private data class Entry(override val key: String, override val name: String, override val brand: String,
		override val plugin: String, override val tags: List<String>, override val description: String,
		val origin: String?) : ModuleText

	/** Same categories, in the same order, as the palette's chips
	 * (ModulePalette.tagCategories): the two pickers should agree on what a
	 * VCO is. */
	private val tagCategories: List<Triple<String, String, (Entry) -> Boolean>> = listOf(
		Triple("VCO", LINE_VCO) { e -> "Oscillator" in e.tags && "Low-frequency oscillator" !in e.tags },
		Triple("LFO", LINE_LFO) { e -> "Low-frequency oscillator" in e.tags },
		Triple("VCF", LINE_VCF) { e -> "Filter" in e.tags },
		Triple("VCA", LINE_VCA) { e -> "Voltage-controlled amplifier" in e.tags },
		Triple("ENV", LINE_ENV) { e -> "Envelope generator" in e.tags },
		Triple("SEQ", LINE_SEQ) { e -> "Sequencer" in e.tags },
		Triple("DRUM", LINE_DRUM) { e -> "Drum" in e.tags },
		Triple("MIX", LINE_MIX) { e -> "Mixer" in e.tags },
		Triple("FX", LINE_FX) { e -> e.tags.any { it in setOf("Effect", "Delay", "Reverb", "Chorus", "Distortion", "Flanger", "Phaser") } },
		Triple("NOISE", LINE_NOISE) { e -> "Noise" in e.tags },
		Triple("QNT", LINE_QNT) { e -> "Quantizer" in e.tags },
		Triple("VISUAL", LINE_VISUAL) { e -> "Visual" in e.tags && "Blank" !in e.tags },
		Triple("MIDI", LINE_MIDI) { e -> "MIDI" in e.tags },
		Triple("UTIL", "") { e -> "Utility" in e.tags },
	)

	private var open: AlertDialog? = null
	private var inkColor = Color.DKGRAY

	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	/** A 24 x 24 path, filled (the Material icons) or stroked (the drawn ones). */
	private class PathIcon(pathData: String, private val stroked: Boolean, color: Int, private val sizePx: Int) : Drawable() {
		private val path = PathParser.createPathFromPathData(pathData)
		private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			this.color = color
			if (stroked) {
				style = Paint.Style.STROKE
				strokeWidth = 2f
				strokeCap = Paint.Cap.ROUND
				strokeJoin = Paint.Join.ROUND
			}
		}
		override fun draw(canvas: Canvas) {
			val s = minOf(bounds.width(), bounds.height()) / 24f
			canvas.save()
			canvas.translate(bounds.left.toFloat(), bounds.top.toFloat())
			canvas.scale(s, s)
			canvas.drawPath(path, paint)
			canvas.restore()
		}
		override fun getIntrinsicWidth() = sizePx
		override fun getIntrinsicHeight() = sizePx
		override fun setAlpha(alpha: Int) { paint.alpha = alpha }
		override fun setColorFilter(filter: ColorFilter?) { paint.colorFilter = filter }
		@Deprecated("Deprecated in Java")
		override fun getOpacity() = PixelFormat.TRANSLUCENT
	}

	private fun fill(pathData: String): Drawable = PathIcon(pathData, false, inkColor, dp(28))
	private fun line(pathData: String): Drawable = PathIcon(pathData, true, inkColor, dp(28))

	/** One of the app's own toolbar icons, in this menu's ink. */
	private fun res(id: Int): Drawable? =
		activity.getDrawable(id)?.mutate()?.also { it.setTint(inkColor) }

	/** The dialog's own text colour, whatever the device's dialog theme is:
	 * the icons are drawn in it, so pictures and words are one ink. */
	private fun resolveInk() {
		val context = AlertDialog.Builder(activity).context
		val tv = TypedValue()
		if (context.theme.resolveAttribute(android.R.attr.textColorPrimary, tv, true))
			inkColor = if (tv.resourceId != 0) context.getColor(tv.resourceId) else tv.data
	}

	fun show(which: Int) {
		resolveInk()
		// One at a time: START pressed again while a menu is up closes it.
		open?.let {
			if (it.isShowing) {
				it.dismiss()
				open = null
				return
			}
		}
		if (which == ADD_MODULE) showCategories() else showMain()
	}

	/** A list to choose from, for another window (ChatPanel). */
	internal fun choose(title: String, rows: List<PadRow>, start: Int, onPick: (Int) -> Unit) {
		resolveInk()
		list(title, rows, start, onPick = onPick)
	}

	/** A row that is the one chosen now, or is not. */
	internal fun choiceRow(label: String, chosen: Boolean, note: String? = null) =
		PadRow(label, line(if (chosen) LINE_CHECKED else LINE_BOX), note = note)

	private fun showMain() {
		val items = listOf<Pair<PadRow, () -> Unit>>(
			if (paused()) PadRow("Play", res(R.drawable.ic_tb_play)) to { togglePause() }
			else PadRow("Pause", res(R.drawable.ic_tb_pause)) to { togglePause() },
			PadRow("Chat…", line(LINE_CHAT)) to { openChat() },
			PadRow("Add module…", res(R.drawable.ic_tb_modules)) to { showCategories() },
			PadRow("Move module", fill(FILL_MOVE)) to { command(CMD_MOVE_MODULE) },
			PadRow("Module menu…", fill(FILL_MODULE_MENU)) to { command(CMD_MODULE_MENU) },
			PadRow("Tags of this module…", line(LINE_TAG)) to { showFocusedTags() },
			PadRow("Delete module", res(R.drawable.ic_tb_delete)) to { command(CMD_DELETE_MODULE) },
			PadRow("XY pad", line(LINE_XY)) to { toggleXyPad() },
			PadRow(if (signalFlow()) "Signal flow: on" else "Signal flow: off", line(LINE_FLOW)) to { setSignalFlow(!signalFlow()) },
			PadRow("Undo", res(R.drawable.ic_tb_undo)) to { history(0) },
			PadRow("Redo", res(R.drawable.ic_tb_redo)) to { history(1) },
			PadRow("File…", fill(FILL_FILE)) to { openRackMenu(0) },
			PadRow("Edit…", fill(FILL_EDIT)) to { openRackMenu(1) },
			PadRow("View…", fill(FILL_VIEW)) to { openRackMenu(2) },
			PadRow("Engine…", fill(FILL_ENGINE)) to { openRackMenu(3) },
			PadRow("Help…", fill(FILL_HELP)) to { openRackMenu(5) },
			PadRow("Tools…", fill(FILL_TOOLS)) to { showTools() },
		)
		list("Menu", items.map { it.first }) { items[it].second() }
	}

	private fun showTools() {
		val views = tools()
		if (views.isEmpty())
			return
		val rows = views.mapIndexed { i, v ->
			// The palette button borrows the View menu's name as its description.
			val label = if (i == 0) "Module palette" else v.contentDescription?.toString() ?: "Tool ${i + 1}"
			// The button's own picture. Its toolbar tint is cream, for a dark
			// bar: on this light sheet it takes the ink instead -- unless the
			// tint says something (the record dot and the bin are red).
			val image = v as? ImageView
			val icon = image?.drawable?.constantState?.newDrawable(activity.resources)?.mutate()
			val tint = image?.imageTintList?.defaultColor
			icon?.setTint(if (tint == null || Color.luminance(tint) > 0.55f) inkColor else tint)
			PadRow(label, icon)
		}
		list("Tools", rows) { views[it].performClick() }
	}

	private fun loadEntries(): List<Entry> {
		ModuleTags.refresh(activity.getExternalFilesDir(null))
		val out = ArrayList<Entry>()
		runCatching {
			val arr = JSONArray(getModelsJson())
			for (i in 0 until arr.length()) {
				val o = arr.getJSONObject(i)
				val tagsJ = o.optJSONArray("tags")
				val tags = ArrayList<String>()
				if (tagsJ != null)
					for (t in 0 until tagsJ.length())
						tags.add(tagsJ.getString(t))
				val key = o.getString("key")
				val description = o.optString("description")
				out.add(Entry(key, o.optString("name"), o.optString("brand"), o.optString("plugin"), tags,
					description, MutableOrigin.of(key, description)))
			}
		}
		return out
	}

	/** What the module pickers show, or null with a word to the user if the
	 * list has not been built yet (the palette has never been opened). */
	private fun entriesOrSay(): List<Entry>? {
		val entries = loadEntries()
		if (entries.isNotEmpty())
			return entries
		runCatching { requestBuild() }
		Toast.makeText(activity, "The module list is still loading. Try again.", Toast.LENGTH_SHORT).show()
		return null
	}

	private fun showCategories(start: Int = 0) {
		val entries = entriesOrSay() ?: return
		val rows = ArrayList<Pair<PadRow, (Int) -> Unit>>()
		rows.add(PadRow("Search…", line(LINE_SEARCH)) to { _ -> showSearch(entries) })
		rows.add(PadRow("Tags…", line(LINE_TAG)) to { _ -> showTagList(entries) })
		fun category(label: String, icon: Drawable?, found: List<Entry>) {
			if (found.isNotEmpty())
				rows.add(PadRow("$label   (${found.size})", icon) to { at ->
					showModules(label, found, byOrigin = label == MUTABLE) { showCategories(at) }
				})
		}
		// The Mutable Instruments ports, whoever made them and whatever they
		// are called: first, because their names are the ones that give no
		// hint of what they are.
		category(MUTABLE, line(LINE_MUTABLE), entries.filter { it.origin != null })
		for ((label, icon, pred) in tagCategories)
			category(label, if (icon.isEmpty()) fill(FILL_TOOLS) else line(icon), entries.filter(pred))
		category("MISC", line(LINE_MISC), entries.filter { e -> tagCategories.none { (_, _, pred) -> pred(e) } })
		category("ALL", res(R.drawable.ic_tb_modules), entries)
		list("Add module", rows.map { it.first }, start) { rows[it].second(it) }
	}

	/** A module's own line under its name: what it is a port of, and the tags
	 * the user gave it. */
	private fun noteOf(e: Entry, byOrigin: Boolean = false): String? {
		val parts = ArrayList<String>()
		if (byOrigin) parts.add(e.brand)
		else e.origin?.let { parts.add("Mutable Instruments $it") }
		val own = ModuleTags.of(e.key)
		if (own.isNotEmpty())
			parts.add(own.joinToString("  ") { "#$it" })
		return if (parts.isEmpty()) null else parts.joinToString("   ")
	}

	private fun showModules(category: String, entries: List<Entry>, byOrigin: Boolean = false, start: Int = 0,
			onBack: () -> Unit) {
		// Among the Mutable ports the hardware's name leads, and orders the
		// list: that is the name being looked for. Everywhere else it is a
		// note under the module's own name.
		val sorted = if (byOrigin)
			entries.sortedWith(compareBy({ it.origin?.lowercase() }, { it.brand.lowercase() }, { it.name.lowercase() }))
		else
			entries.sortedWith(compareBy({ it.name.lowercase() }, { it.brand.lowercase() }))
		val rows = sorted.map {
			PadRow(if (byOrigin) "${it.origin}   ·   ${it.name}" else "${it.name}   ·   ${it.brand}",
				thumbKey = it.key, note = noteOf(it, byOrigin))
		}
		list("Add module: $category", rows, start, hint = "A  add      Y  tags      B  back",
			onY = { at -> showTags(sorted[at]) { showModules(category, entries, byOrigin, at, onBack) } },
			onBack = onBack) { add(sorted[it]) }
	}

	private fun add(e: Entry) {
		// Where the palette puts a tapped module: the middle of the rack
		// that is on screen. The focus moves to it (gamepad_nav.cpp).
		val v = activity.window.decorView
		runCatching { chooseAt(e.key, v.width * 0.5f, v.height * 0.55f) }
	}

	/** Looking for a module by name, maker or tag. */
	private fun showSearch(entries: List<Entry>, initial: String = "", startRow: Int = -1) {
		var found: List<Entry> = emptyList()
		val d = PadTextDialog(activity, inkColor, "Search", "name, maker or tag:  scope, 波形 …", "results",
			"D-pad  keys      A  type      Y  erase      X  keys / results      on a result:  A  add   Y  tags      B  back",
			initial, startRow)
		d.rowsFor = { query ->
			found = ModuleTags.search(entries, query)
			found.map { e ->
				val tags = e.tags.joinToString(", ")
				PadRow("${e.name}   ·   ${e.brand}", thumbKey = e.key,
					note = listOfNotNull(noteOf(e), tags.ifEmpty { null }).joinToString("   ").ifEmpty { null })
			}
		}
		d.onPick = { add(found[it]) }
		d.onY = { at ->
			val query = d.text
			showTags(found[at]) { showSearch(entries, query, at) }
		}
		d.onBack = { showCategories() }
		open = d.show()
	}

	/** Every tag there is, the user's own first. */
	private fun showTagList(entries: List<Entry>, start: Int = 0) {
		val rows = ArrayList<Pair<PadRow, (Int) -> Unit>>()
		for ((tag, _) in ModuleTags.inUse()) {
			val found = entries.filter { tag in ModuleTags.of(it.key) }
			if (found.isNotEmpty())
				rows.add(PadRow("#$tag   (${found.size})", line(LINE_TAG)) to { at ->
					showModules("#$tag", found) { showTagList(loadEntries(), at) }
				})
		}
		for (tag in ModuleTags.builtIns) {
			val found = entries.filter { tag.name in it.tags }
			if (found.isNotEmpty())
				rows.add(PadRow("${tag.name}   (${found.size})", note = tag.ja.takeIf { it != tag.name }) to { at ->
					showModules(tag.name, found) { showTagList(loadEntries(), at) }
				})
		}
		list("Tags", rows.map { it.first }, start, hint = "Your own tags first.  Y on a module gives it one.",
			onBack = { showCategories(1) }) { rows[it].second(it) }
	}

	private fun showFocusedTags() {
		val key = focusedModel()
		val entry = (entriesOrSay() ?: return).firstOrNull { it.key == key }
		if (entry == null)
			Toast.makeText(activity, "Move the focus to a module first", Toast.LENGTH_SHORT).show()
		else
			showTags(entry) {}
	}

	/** The user's tags, to put on a module or take off it. */
	private fun showTags(e: Entry, start: Int = 0, onBack: () -> Unit) {
		val mine = ModuleTags.of(e.key)
		val used = ModuleTags.inUse().map { it.first }
		val rows = listOf(PadRow("New tag…", line(LINE_PLUS))) +
			used.map { PadRow("#$it", line(if (it in mine) LINE_CHECKED else LINE_BOX)) }
		val builtIn = if (e.tags.isEmpty()) "" else "Rack's own:  ${e.tags.joinToString(", ")}"
		list("Tags of ${e.name}", rows, start, hint = builtIn.ifEmpty { "A  put on / take off      B  back" },
			onBack = onBack) { i ->
			if (i == 0) {
				val d = PadTextDialog(activity, inkColor, "New tag", "a word of your own", "OK",
					"D-pad  keys      A  type      Y  erase      START  OK      B  back")
				d.onDone = { text ->
					val tag = ModuleTags.normalize(text)
					ModuleTags.set(e.key, tag, true)
					showTags(e, maxOf(0, ModuleTags.inUse().indexOfFirst { it.first == tag } + 1), onBack)
				}
				d.onBack = { showTags(e, 0, onBack) }
				open = d.show()
			}
			else {
				ModuleTags.set(e.key, used[i - 1], used[i - 1] !in mine)
				// Taken off the last module that had it, a tag is gone.
				showTags(e, i, onBack)
			}
		}
	}

	private fun titleView(context: Context, title: String, hint: String?): View = LinearLayout(context).apply {
		orientation = LinearLayout.VERTICAL
		setPadding(dp(24), dp(18), dp(24), dp(10))
		addView(TextView(context).apply {
			text = title
			setTextColor(inkColor)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 20f)
			setTypeface(typeface, android.graphics.Typeface.BOLD)
		})
		if (hint != null) addView(TextView(context).apply {
			text = hint
			setTextColor((inkColor and 0x00FFFFFF) or (0xB0 shl 24))
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
		})
	}

	/** @param start the row the cursor begins on.
	 * @param hint a smaller line under the title.
	 * @param onY what Y does to the row under the cursor, if anything.
	 * @param onBack where B goes, if not out of the menus. */
	private fun list(title: String, rows: List<PadRow>, start: Int = 0, hint: String? = null,
			onY: ((Int) -> Unit)? = null, onBack: (() -> Unit)? = null, onPick: (Int) -> Unit) {
		val builder = AlertDialog.Builder(activity)
		val adapter = PadListAdapter(activity, builder.context, rows, inkColor)
		val dialog = builder
			.setCustomTitle(titleView(builder.context, title, hint))
			.setAdapter(adapter) { _, i -> onPick(i) }
			.create()
		adapter.cursor = start.coerceIn(0, maxOf(0, rows.size - 1))
		fun moveCursor(to: Int) {
			dialog.listView?.let { adapter.move(it, to) }
		}
		// A release only counts if this dialog saw the press. The button that
		// opened the menu is still down when the menu appears, and its release
		// arrives here: taken at face value, letting go of START closed the
		// menu that pressing it had opened.
		val pressedHere = HashSet<Int>()
		dialog.setOnKeyListener { _, keyCode, event ->
			val lv = dialog.listView
			val down = event.action == KeyEvent.ACTION_DOWN
			val ours = keyCode == KeyEvent.KEYCODE_BUTTON_A || keyCode == KeyEvent.KEYCODE_BUTTON_B ||
				keyCode == KeyEvent.KEYCODE_BUTTON_START || keyCode == KeyEvent.KEYCODE_BUTTON_X ||
				keyCode == KeyEvent.KEYCODE_BUTTON_L1 || keyCode == KeyEvent.KEYCODE_BUTTON_R1
			if (ours) {
				if (down) {
					// A held button repeats; only its first press is a press.
					if (event.repeatCount == 0)
						pressedHere.add(keyCode)
					else if (keyCode !in pressedHere)
						return@setOnKeyListener true
				}
				else if (!pressedHere.remove(keyCode)) {
					return@setOnKeyListener true
				}
			}
			when (keyCode) {
				// The D-pad, whether it arrives as keys or (as on the Nova)
				// as the HAT axis Android turns into these keys.
				KeyEvent.KEYCODE_DPAD_UP, KeyEvent.KEYCODE_DPAD_DOWN -> {
					if (down)
						moveCursor(adapter.cursor + if (keyCode == KeyEvent.KEYCODE_DPAD_UP) -1 else 1)
					true
				}
				KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER, KeyEvent.KEYCODE_ENTER -> {
					// On release, as a tap is: the next dialog must not open
					// with A still down.
					if (!down && rows.isNotEmpty()) {
						dialog.dismiss()
						onPick(adapter.cursor)
					}
					true
				}
				KeyEvent.KEYCODE_BUTTON_B -> {
					if (!down) {
						dialog.dismiss()
						onBack?.invoke()
					}
					true
				}
				KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_X -> {
					if (!down)
						dialog.dismiss()
					true
				}
				KeyEvent.KEYCODE_BUTTON_Y -> {
					if (!down && onY != null && rows.isNotEmpty()) {
						dialog.dismiss()
						onY(adapter.cursor)
					}
					true
				}
				KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 -> {
					if (down && lv != null && lv.count > 0) {
						val page = maxOf(1, lv.lastVisiblePosition - lv.firstVisiblePosition)
						moveCursor(adapter.cursor + if (keyCode == KeyEvent.KEYCODE_BUTTON_L1) -page else page)
					}
					true
				}
				else -> false
			}
		}
		dialog.setOnDismissListener { if (open === dialog) open = null }
		open = dialog
		dialog.show()
		// One highlight, not two: the list's own selection would sit on
		// whichever row it last landed on while the cursor moved elsewhere.
		dialog.listView?.selector = android.graphics.drawable.ColorDrawable(Color.TRANSPARENT)
		if (adapter.cursor > 0)
			dialog.listView?.setSelectionFromTop(adapter.cursor, dp(64))
	}
}
