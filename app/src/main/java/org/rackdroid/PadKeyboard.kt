package org.rackdroid

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.view.MotionEvent
import android.view.View

/**
 * A keyboard for the D-pad: Android's own cannot be reached with a controller
 * on this device. Letters, digits and what a search needs, in four rows; a
 * finger can press it too. Anything else (Japanese, say) is typed with
 * Android's keyboard, which a touch on the text field brings up.
 */
class PadKeyboard(
	context: Context,
	private val ink: Int,
	/** What the last key of the last row says and does. */
	private val doneLabel: String,
	private val onText: (String) -> Unit,
	private val onErase: () -> Unit,
	private val onDone: () -> Unit,
) : View(context) {
	private companion object {
		const val ERASE = "\b"
		const val DONE = "\n"
	}

	private val rows: List<List<String>> = listOf(
		"1234567890".map { it.toString() },
		"qwertyuiop".map { it.toString() },
		"asdfghjkl-".map { it.toString() },
		"zxcvbnm".map { it.toString() } + listOf(" ", ERASE, DONE),
	)
	private var row = 1
	private var column = 0
	/** Whether the D-pad is on the keyboard; elsewhere, no key is lit. */
	var active = true
		set(value) {
			field = value
			invalidate()
		}

	private val density = resources.displayMetrics.density
	private val paint = Paint(Paint.ANTI_ALIAS_FLAG)
	private val rect = RectF()

	fun press() {
		when (val k = rows[row][column]) {
			ERASE -> onErase()
			DONE -> onDone()
			else -> onText(k)
		}
	}

	/** Moves between keys; false if that would leave the top of the keyboard. */
	fun move(dx: Int, dy: Int): Boolean {
		if (row + dy < 0)
			return false
		row = (row + dy).coerceAtMost(rows.size - 1)
		val n = rows[row].size
		column = ((column + dx) % n + n) % n
		invalidate()
		return true
	}

	override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
		setMeasuredDimension(MeasureSpec.getSize(widthMeasureSpec), (rows.size * 38 * density).toInt())
	}

	private fun keyRect(r: Int, c: Int) {
		val w = width.toFloat() / rows[r].size
		val h = height.toFloat() / rows.size
		val gap = 2 * density
		rect.set(c * w + gap, r * h + gap, (c + 1) * w - gap, (r + 1) * h - gap)
	}

	override fun onDraw(canvas: Canvas) {
		val radius = 7 * density
		for (r in rows.indices) {
			for (c in rows[r].indices) {
				keyRect(r, c)
				val lit = active && r == row && c == column
				paint.style = Paint.Style.FILL
				paint.color = (ink and 0x00FFFFFF) or ((if (lit) 0x60 else 0x14) shl 24)
				canvas.drawRoundRect(rect, radius, radius, paint)
				if (lit) {
					paint.style = Paint.Style.STROKE
					paint.strokeWidth = 2 * density
					paint.color = ink
					canvas.drawRoundRect(rect, radius, radius, paint)
					paint.style = Paint.Style.FILL
				}
				paint.color = if (isEnabled) ink else Color.GRAY
				paint.textAlign = Paint.Align.CENTER
				val label = when (val k = rows[r][c]) {
					" " -> "space"
					ERASE -> "⌫"
					DONE -> doneLabel
					else -> k
				}
				paint.textSize = (if (label.length > 1) 12 else 17) * density
				canvas.drawText(label, rect.centerX(), rect.centerY() - (paint.ascent() + paint.descent()) / 2, paint)
			}
		}
	}

	override fun onTouchEvent(event: MotionEvent): Boolean {
		if (event.actionMasked == MotionEvent.ACTION_UP) {
			val r = (event.y / height * rows.size).toInt().coerceIn(0, rows.size - 1)
			val c = (event.x / width * rows[r].size).toInt().coerceIn(0, rows[r].size - 1)
			row = r
			column = c
			invalidate()
			press()
		}
		return true
	}
}
