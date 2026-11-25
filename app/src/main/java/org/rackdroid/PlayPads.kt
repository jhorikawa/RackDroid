package org.rackdroid

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.view.MotionEvent
import android.view.View

/** Eight pads to drum on with several fingers, CC 64..71 of the "Gamepad" MIDI
 * driver, and how high on the pad it was hit, 72..79 (gamepad_midi.cpp). */
class DrumPads(context: Context, private val onHit: (Int, Float, Boolean) -> Unit) : View(context) {
	companion object {
		const val COLUMNS = 4
		const val ROWS = 2
	}

	/** The pad each finger holds. */
	private val held = HashMap<Int, Int>()
	private val level = FloatArray(COLUMNS * ROWS)
	private val density = resources.displayMetrics.density
	private val ground = Paint().apply { color = AppTheme.withAlpha(AppTheme.current.surface, 92) }
	private val off = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = AppTheme.withAlpha(AppTheme.current.textPrimary, 14) }
	private val on = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = AppTheme.current.accent }
	private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = AppTheme.withAlpha(AppTheme.current.textPrimary, 70)
		textSize = 13f * density
		typeface = AppFont.get(context)
		textAlign = Paint.Align.CENTER
	}

	override fun onDraw(canvas: Canvas) {
		canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), ground)
		val w = width.toFloat() / COLUMNS
		val h = height.toFloat() / ROWS
		val gap = 4f * density
		for (i in 0 until COLUMNS * ROWS) {
			val x = w * (i % COLUMNS)
			// 1 to 4 below, as drum machines have them.
			val y = h * (ROWS - 1 - i / COLUMNS)
			canvas.drawRoundRect(x + gap, y + gap, x + w - gap, y + h - gap, 8f * density, 8f * density, if (held.containsValue(i)) on else off)
			canvas.drawText("${i + 1}", x + w / 2, y + h / 2 + 5f * density, label)
		}
	}

	private fun padAt(x: Float, y: Float): Int {
		val column = (x / width.coerceAtLeast(1) * COLUMNS).toInt().coerceIn(0, COLUMNS - 1)
		val row = (y / height.coerceAtLeast(1) * ROWS).toInt().coerceIn(0, ROWS - 1)
		return (ROWS - 1 - row) * COLUMNS + column
	}

	private fun hit(finger: Int, x: Float, y: Float) {
		val pad = padAt(x, y)
		val h = height.coerceAtLeast(1).toFloat() / ROWS
		level[pad] = (1f - (y % h) / h).coerceIn(0.02f, 1f)
		held[finger] = pad
		onHit(pad, level[pad], true)
	}

	private fun lift(finger: Int) {
		val pad = held.remove(finger) ?: return
		if (!held.containsValue(pad))
			onHit(pad, level[pad], false)
	}

	fun release() {
		held.keys.toList().forEach { lift(it) }
		invalidate()
	}

	override fun onTouchEvent(event: MotionEvent): Boolean {
		val i = event.actionIndex
		when (event.actionMasked) {
			MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> hit(event.getPointerId(i), event.getX(i), event.getY(i))
			MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> lift(event.getPointerId(i))
			MotionEvent.ACTION_CANCEL -> release()
			else -> return true
		}
		invalidate()
		return true
	}
}

/** Four faders, CC 80..83. They stay where they are put. */
class Faders(context: Context, private val onMove: (Int, Float) -> Unit) : View(context) {
	companion object {
		const val COUNT = 4
		/** Kept for when the surface comes back. */
		private val at = FloatArray(COUNT)
	}

	private val density = resources.displayMetrics.density
	private val ground = Paint().apply { color = AppTheme.withAlpha(AppTheme.current.surface, 92) }
	private val track = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = AppTheme.withAlpha(AppTheme.current.textPrimary, 14) }
	private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = AppTheme.current.accent }

	override fun onDraw(canvas: Canvas) {
		canvas.drawRect(0f, 0f, width.toFloat(), height.toFloat(), ground)
		val w = width.toFloat() / COUNT
		val gap = 5f * density
		for (i in 0 until COUNT) {
			canvas.drawRoundRect(w * i + gap, gap, w * (i + 1) - gap, height - gap, 8f * density, 8f * density, track)
			val y = gap + (1f - at[i]) * (height - 2 * gap)
			canvas.drawRoundRect(w * i + gap, y, w * (i + 1) - gap, height - gap, 8f * density, 8f * density, fill)
		}
	}

	override fun onTouchEvent(event: MotionEvent): Boolean {
		if (event.actionMasked != MotionEvent.ACTION_UP && event.actionMasked != MotionEvent.ACTION_CANCEL) {
			for (p in 0 until event.pointerCount) {
				val i = (event.getX(p) / width.coerceAtLeast(1) * COUNT).toInt().coerceIn(0, COUNT - 1)
				at[i] = (1f - event.getY(p) / height.coerceAtLeast(1)).coerceIn(0f, 1f)
				onMove(i, at[i])
			}
		}
		invalidate()
		return true
	}
}
