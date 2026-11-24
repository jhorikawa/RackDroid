package org.rackdroid

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.BitmapDrawable
import android.graphics.drawable.Drawable
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.text.style.RelativeSizeSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.BaseAdapter
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView

/** One row of a controller menu (GamepadMenu, ModuleSearch). A module row has
 * no icon of its own: its panel is looked up when the row comes on screen. A
 * row may carry a second, smaller line under its label. */
internal class PadRow(val label: String, val icon: Drawable? = null, val thumbKey: String? = null,
	val note: String? = null)

internal class PadListAdapter(
	private val activity: Activity,
	private val context: Context,
	private val rows: List<PadRow>,
	private val inkColor: Int,
) : BaseAdapter() {
	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()
	private val modules = rows.any { it.thumbKey != null }
	/** A module's panel is tall and narrow: the picture column is sized
	 * for that, so every name starts at the same x whatever the width. */
	private val iconW = if (modules) dp(64) else dp(28)
	private val iconH = if (modules) dp(56) else dp(28)

	/** The row the controller is on. Kept here and drawn as the row's
	 * background, not left to the list's own selection: a list opened
	 * while the window is in touch mode shows no selection until a key
	 * arrives, so the first press of the D-pad was spent making one
	 * appear, and A straight after opening did nothing. */
	var cursor = 0
	/** False while the D-pad is somewhere else in the window. */
	var lit = true
	private val cursorColor = (inkColor and 0x00FFFFFF) or 0x40000000
	private val noteColor = (inkColor and 0x00FFFFFF) or (0xB0 shl 24)

	fun paint(row: View, position: Int) =
		row.setBackgroundColor(if (lit && position == cursor) cursorColor else Color.TRANSPARENT)

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
		paint(row, position)
		val r = rows[position]
		val image = row.getChildAt(0) as ImageView
		val text = row.getChildAt(1) as TextView
		text.text = if (r.note == null) r.label else SpannableString("${r.label}\n${r.note}").apply {
			val from = r.label.length + 1
			setSpan(RelativeSizeSpan(0.78f), from, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
			setSpan(ForegroundColorSpan(noteColor), from, length, Spanned.SPAN_EXCLUSIVE_EXCLUSIVE)
		}
		if (r.thumbKey != null) {
			// Decoded on demand and cached, as the palette's tiles are. A
			// module with no art leaves its place empty.
			val bmp = runCatching { ThumbnailCache.get(activity.filesDir, r.thumbKey, iconW) }.getOrNull()
			image.setImageDrawable(bmp?.let { BitmapDrawable(activity.resources, it) })
		}
		else {
			image.setImageDrawable(r.icon)
		}
		// A dialog's list measures its first row to find out how tall it is
		// and lays that view out as measured, at a width that is not the
		// list's: the highlight stopped half way across. Measured again.
		row.forceLayout()
		return row
	}

	/** Lights or puts out the cursor. */
	fun light(lv: ListView, on: Boolean) {
		lit = on
		lv.getChildAt(cursor - lv.firstVisiblePosition)?.let { paint(it, cursor) }
	}

	/** Moves the cursor, and the list with it only as far as that takes.
	 *
	 * The highlight is moved by repainting the two rows it concerns, never by
	 * notifyDataSetChanged(): that lays the list out again around its OWN
	 * selection -- row 0, from the moment the D-pad takes the window out of
	 * touch mode -- so every press threw the list back to the top and scrolled
	 * it down again from there. The list's selection is kept on the cursor for
	 * the same reason. */
	fun move(lv: ListView, to: Int) {
		if (rows.isEmpty())
			return
		val from = cursor
		cursor = to.coerceIn(0, rows.size - 1)
		if (cursor == from)
			return
		lv.getChildAt(from - lv.firstVisiblePosition)?.let { paint(it, from) }
		val row = lv.getChildAt(cursor - lv.firstVisiblePosition)
		row?.let { paint(it, cursor) }
		val room = lv.height - lv.paddingTop - lv.paddingBottom
		// Where the row goes: where it is if that is on screen, or else just
		// inside the edge it has gone past.
		val height = (row ?: lv.getChildAt(0))?.height ?: 0
		val top = when {
			row != null -> (row.top - lv.paddingTop).coerceIn(0, maxOf(0, room - height))
			cursor > from -> maxOf(0, room - height)
			else -> 0
		}
		lv.setSelectionFromTop(cursor, top)
	}
}
