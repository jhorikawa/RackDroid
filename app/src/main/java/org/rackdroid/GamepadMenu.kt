package org.rackdroid

import android.app.Activity
import android.app.AlertDialog
import android.view.KeyEvent
import android.view.View
import android.widget.Toast
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
) {
	companion object {
		const val MAIN = 0
		const val ADD_MODULE = 1

		/** Matches GamepadCommand in gamepad_nav.cpp. */
		const val CMD_MODULE_MENU = 1
	}

	private data class Entry(val key: String, val name: String, val brand: String, val tags: List<String>)

	/** Same categories, in the same order, as the palette's chips
	 * (ModulePalette.tagCategories): the two pickers should agree on what a
	 * VCO is. */
	private val tagCategories: List<Pair<String, (Entry) -> Boolean>> = listOf(
		"VCO" to { e -> "Oscillator" in e.tags && "Low-frequency oscillator" !in e.tags },
		"LFO" to { e -> "Low-frequency oscillator" in e.tags },
		"VCF" to { e -> "Filter" in e.tags },
		"VCA" to { e -> "Voltage-controlled amplifier" in e.tags },
		"ENV" to { e -> "Envelope generator" in e.tags },
		"SEQ" to { e -> "Sequencer" in e.tags },
		"DRUM" to { e -> "Drum" in e.tags },
		"MIX" to { e -> "Mixer" in e.tags },
		"FX" to { e -> e.tags.any { it in setOf("Effect", "Delay", "Reverb", "Chorus", "Distortion", "Flanger", "Phaser") } },
		"NOISE" to { e -> "Noise" in e.tags },
		"QNT" to { e -> "Quantizer" in e.tags },
		"MIDI" to { e -> "MIDI" in e.tags },
		"UTIL" to { e -> "Utility" in e.tags },
	)

	private var open: AlertDialog? = null

	fun show(which: Int) {
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

	private fun showMain() {
		val items = listOf<Pair<String, () -> Unit>>(
			"Add module…" to { showCategories() },
			"Module menu…" to { command(CMD_MODULE_MENU) },
			"Undo" to { history(0) },
			"Redo" to { history(1) },
			"File…" to { openRackMenu(0) },
			"Edit…" to { openRackMenu(1) },
			"View…" to { openRackMenu(2) },
			"Engine…" to { openRackMenu(3) },
			"Help…" to { openRackMenu(5) },
			"Tools…" to { showTools() },
		)
		list("Menu", items.map { it.first }) { items[it].second() }
	}

	private fun showTools() {
		val views = tools()
		if (views.isEmpty())
			return
		val labels = views.mapIndexed { i, v ->
			// The palette button borrows the View menu's name as its description.
			if (i == 0) "Module palette" else v.contentDescription?.toString() ?: "Tool ${i + 1}"
		}
		list("Tools", labels) { views[it].performClick() }
	}

	private fun loadEntries(): List<Entry> {
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
				out.add(Entry(o.getString("key"), o.optString("name"), o.optString("brand"), tags))
			}
		}
		return out
	}

	private fun showCategories() {
		val entries = loadEntries()
		if (entries.isEmpty()) {
			// Not built yet (the palette has never been opened): ask for it and
			// say so, rather than show an empty list.
			runCatching { requestBuild() }
			Toast.makeText(activity, "The module list is still loading. Try again.", Toast.LENGTH_SHORT).show()
			return
		}
		val cats = ArrayList<Pair<String, List<Entry>>>()
		for ((label, pred) in tagCategories) {
			val found = entries.filter(pred)
			if (found.isNotEmpty())
				cats.add(label to found)
		}
		val misc = entries.filter { e -> tagCategories.none { (_, pred) -> pred(e) } }
		if (misc.isNotEmpty())
			cats.add("MISC" to misc)
		cats.add("ALL" to entries)
		list("Add module", cats.map { "${it.first}   (${it.second.size})" }) { showModules(cats[it].first, cats[it].second) }
	}

	private fun showModules(category: String, entries: List<Entry>) {
		val sorted = entries.sortedWith(compareBy({ it.name.lowercase() }, { it.brand.lowercase() }))
		list("Add module: $category", sorted.map { "${it.name}   ·   ${it.brand}" }) {
			// Where the palette puts a tapped module: the middle of the rack
			// that is on screen. The focus moves to it (gamepad_nav.cpp).
			val v = activity.window.decorView
			runCatching { chooseAt(sorted[it].key, v.width * 0.5f, v.height * 0.55f) }
		}
	}

	private fun list(title: String, labels: List<String>, onPick: (Int) -> Unit) {
		val dialog = AlertDialog.Builder(activity)
			.setTitle(title)
			.setItems(labels.toTypedArray()) { _, i -> onPick(i) }
			.create()
		dialog.setOnKeyListener { _, keyCode, event ->
			val lv = dialog.listView
			val down = event.action == KeyEvent.ACTION_DOWN
			when (keyCode) {
				KeyEvent.KEYCODE_BUTTON_A -> {
					// On release, as a tap is: the next dialog must not open
					// with A still down.
					if (!down && lv != null) {
						val pos = lv.selectedItemPosition
						if (pos >= 0)
							lv.performItemClick(lv.selectedView, pos, lv.getItemIdAtPosition(pos))
					}
					true
				}
				KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_X -> {
					if (!down)
						dialog.dismiss()
					true
				}
				KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 -> {
					if (down && lv != null && lv.count > 0) {
						val page = maxOf(1, lv.lastVisiblePosition - lv.firstVisiblePosition)
						val from = if (lv.selectedItemPosition >= 0) lv.selectedItemPosition else lv.firstVisiblePosition
						val to = if (keyCode == KeyEvent.KEYCODE_BUTTON_L1) from - page else from + page
						lv.setSelection(to.coerceIn(0, lv.count - 1))
					}
					true
				}
				else -> false
			}
		}
		dialog.setOnDismissListener { if (open === dialog) open = null }
		open = dialog
		dialog.show()
		// A list shown from a button should start with its first row
		// selected, not wait for a D-pad press to leave touch mode.
		dialog.listView?.let {
			it.isFocusableInTouchMode = true
			it.requestFocusFromTouch()
			it.setSelection(0)
		}
	}
}
