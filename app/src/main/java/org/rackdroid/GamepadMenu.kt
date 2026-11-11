package org.rackdroid

import android.app.Activity
import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.PixelFormat
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
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
) {
	companion object {
		const val MAIN = 0
		const val ADD_MODULE = 1

		/** Matches GamepadCommand in gamepad_nav.cpp. */
		const val CMD_MODULE_MENU = 1
		const val CMD_MOVE_MODULE = 2

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
		private const val LINE_MISC = "M5,12 h0.01 M12,12 h0.01 M19,12 h0.01"
	}

	private data class Entry(val key: String, val name: String, val brand: String, val tags: List<String>)

	/** One row of a menu. A module row has no icon of its own: its panel is
	 * looked up when the row comes on screen. */
	private class Row(val label: String, val icon: Drawable? = null, val thumbKey: String? = null)

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

	private fun showMain() {
		val items = listOf<Pair<Row, () -> Unit>>(
			Row("Add module…", res(R.drawable.ic_tb_modules)) to { showCategories() },
			Row("Move module", fill(FILL_MOVE)) to { command(CMD_MOVE_MODULE) },
			Row("Module menu…", fill(FILL_MODULE_MENU)) to { command(CMD_MODULE_MENU) },
			Row("XY pad", line(LINE_XY)) to { toggleXyPad() },
			Row("Undo", res(R.drawable.ic_tb_undo)) to { history(0) },
			Row("Redo", res(R.drawable.ic_tb_redo)) to { history(1) },
			Row("File…", fill(FILL_FILE)) to { openRackMenu(0) },
			Row("Edit…", fill(FILL_EDIT)) to { openRackMenu(1) },
			Row("View…", fill(FILL_VIEW)) to { openRackMenu(2) },
			Row("Engine…", fill(FILL_ENGINE)) to { openRackMenu(3) },
			Row("Help…", fill(FILL_HELP)) to { openRackMenu(5) },
			Row("Tools…", fill(FILL_TOOLS)) to { showTools() },
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
			Row(label, icon)
		}
		list("Tools", rows) { views[it].performClick() }
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
		val cats = ArrayList<Triple<String, Drawable?, List<Entry>>>()
		for ((label, icon, pred) in tagCategories) {
			val found = entries.filter(pred)
			if (found.isNotEmpty())
				cats.add(Triple(label, if (icon.isEmpty()) fill(FILL_TOOLS) else line(icon), found))
		}
		val misc = entries.filter { e -> tagCategories.none { (_, _, pred) -> pred(e) } }
		if (misc.isNotEmpty())
			cats.add(Triple("MISC", line(LINE_MISC), misc))
		cats.add(Triple("ALL", res(R.drawable.ic_tb_modules), entries))
		list("Add module", cats.map { Row("${it.first}   (${it.third.size})", it.second) }) {
			showModules(cats[it].first, cats[it].third)
		}
	}

	private fun showModules(category: String, entries: List<Entry>) {
		val sorted = entries.sortedWith(compareBy({ it.name.lowercase() }, { it.brand.lowercase() }))
		list("Add module: $category", sorted.map { Row("${it.name}   ·   ${it.brand}", thumbKey = it.key) }) {
			// Where the palette puts a tapped module: the middle of the rack
			// that is on screen. The focus moves to it (gamepad_nav.cpp).
			val v = activity.window.decorView
			runCatching { chooseAt(sorted[it].key, v.width * 0.5f, v.height * 0.55f) }
		}
	}

	private inner class RowAdapter(private val context: Context, private val rows: List<Row>) : BaseAdapter() {
		private val modules = rows.any { it.thumbKey != null }
		/** A module's panel is tall and narrow: the picture column is sized
		 * for that, so every name starts at the same x whatever the width. */
		private val iconW = if (modules) dp(64) else dp(28)
		private val iconH = if (modules) dp(56) else dp(28)

		override fun getCount() = rows.size
		override fun getItem(position: Int): Any = rows[position]
		override fun getItemId(position: Int) = position.toLong()

		override fun getView(position: Int, convertView: View?, parent: ViewGroup?): View {
			val row = convertView as? LinearLayout ?: LinearLayout(context).apply {
				orientation = LinearLayout.HORIZONTAL
				gravity = Gravity.CENTER_VERTICAL
				minimumHeight = dp(if (modules) 64 else 52)
				setPadding(dp(24), dp(4), dp(24), dp(4))
				addView(ImageView(context).apply {
					scaleType = ImageView.ScaleType.FIT_CENTER
					layoutParams = LinearLayout.LayoutParams(iconW, iconH)
				})
				addView(TextView(context).apply {
					setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f)
					setTextColor(inkColor)
					layoutParams = LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
						.apply { marginStart = dp(20) }
				})
			}
			val r = rows[position]
			val image = row.getChildAt(0) as ImageView
			val text = row.getChildAt(1) as TextView
			text.text = r.label
			if (r.thumbKey != null) {
				// Decoded on demand and cached, as the palette's tiles are. A
				// module with no art leaves its place empty.
				val bmp = runCatching { ThumbnailCache.get(activity.filesDir, r.thumbKey, iconW) }.getOrNull()
				image.setImageDrawable(bmp?.let { BitmapDrawable(activity.resources, it) })
			}
			else {
				image.setImageDrawable(r.icon)
			}
			return row
		}
	}

	private fun list(title: String, rows: List<Row>, onPick: (Int) -> Unit) {
		val builder = AlertDialog.Builder(activity)
		val dialog = builder
			.setTitle(title)
			.setAdapter(RowAdapter(builder.context, rows)) { _, i -> onPick(i) }
			.create()
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
