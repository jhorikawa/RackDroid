package org.rackdroid

import android.app.Activity
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.PopupWindow
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * A touch pad over the bottom of the rack: where the finger is becomes two
 * control voltages, and whether it is down a gate (gamepad_midi.cpp sends them
 * as CC 23, 24 and 25 of the "Gamepad" MIDI driver).
 *
 *   x  0 at the left edge, 1 at the right
 *   y  0 at the bottom edge, 1 at the top
 *
 * Lifting the finger drops the gate and leaves x and y where they were, as a
 * hardware pad does: a note's pitch should not jump as it is released.
 *
 * Shown the way the on-screen piano is: a popup that never takes key focus,
 * so the controller keeps working the rack above it.
 */
class XyPad(
	private val activity: Activity,
	/** Pixels to stay clear of at the bottom of the window: the module
	 * palette sits there, and a popup shown later does not go over it. */
	private val bottomClearance: () -> Int,
	/** Pixels on the right that the chat covers. */
	private val rightCovered: () -> Int,
	/** finger 0 or 1 */
	private val onPad: (Int, Float, Float, Boolean) -> Unit,
	private val onDrum: (Int, Float, Boolean) -> Unit,
	private val onFader: (Int, Float) -> Unit,
) {
	private var popup: PopupWindow? = null

	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	val showing get() = popup != null

	companion object {
		const val XY = 1
		const val DRUMS = 2
		const val FADERS = 4
	}

	private var drums: DrumPads? = null
	/** Put up by play mode, and taken away with it. */
	private var byPlay = false

	fun toggle(which: Int) {
		if (popup != null) close() else open(which)
	}

	/** Play mode came or went, with a patch that uses these. */
	fun play(which: Int) {
		if (which != 0 && popup == null) {
			open(which)
			byPlay = true
		} else if (which == 0 && byPlay)
			close()
	}

	/** Side by side, if it is more than one. */
	private fun open(which: Int) {
		byPlay = false
		val pad = android.widget.LinearLayout(activity)
		fun put(v: View, weight: Float) = pad.addView(v, android.widget.LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, weight))
		if (which and DRUMS != 0)
			put(DrumPads(activity, onDrum).also { drums = it }, 1.3f)
		if (which and XY != 0)
			put(PadView(activity), 1f)
		if (which and FADERS != 0)
			put(Faders(activity, onFader), 0.7f)
		val closeBtn = ImageButton(activity).apply {
			setImageResource(R.drawable.ic_tb_close)
			imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.textPrimary)
			scaleType = ImageView.ScaleType.FIT_CENTER
			contentDescription = activity.getString(android.R.string.cancel)
			setPadding(dp(6), dp(6), dp(6), dp(6))
			background = GradientDrawable().apply {
				shape = GradientDrawable.OVAL
				setColor(AppTheme.withAlpha(Color.BLACK, 70))
			}
			setOnClickListener { close() }
		}
		val holder = FrameLayout(activity).apply {
			addView(pad, FrameLayout.LayoutParams(
				ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
			addView(closeBtn, FrameLayout.LayoutParams(dp(34), dp(34)).apply {
				gravity = Gravity.TOP or Gravity.END
				setMargins(0, dp(6), dp(6), 0)
			})
		}
		val p = PopupWindow(holder, activity.window.decorView.width - rightCovered(), dp(200))
		p.isFocusable = false // never steal keys from the canvas
		popup = p
		val decor = activity.window.decorView
		decor.post {
			runCatching {
				val bottom = ViewCompat.getRootWindowInsets(decor)?.getInsets(
					WindowInsetsCompat.Type.systemBars())?.bottom ?: 0
				// Above the palette rather than under it: under it, the lower
				// part of the pad could not be touched.
				p.showAtLocation(decor, Gravity.BOTTOM or Gravity.START, 0,
					maxOf(bottom, runCatching { bottomClearance() }.getOrDefault(0)))
			}
		}
	}

	fun close() {
		popup?.dismiss()
		popup = null
		byPlay = false
		// A finger still down when the pad goes away never sends its lift.
		lift()
	}

	private fun lift() {
		for (f in 0..1)
			onPad(f, lastX[f], lastY[f], false)
		drums?.release()
	}

	/** The app left the screen with a finger possibly still on the pad. */
	fun release() {
		if (popup != null)
			lift()
	}

	private val lastX = FloatArray(2)
	private val lastY = FloatArray(2)

	private inner class PadView(context: Context) : View(context) {
		/** Which finger is which, by Android's numbers of them, or -1. */
		private val fingers = intArrayOf(-1, -1)
		private val ink = AppTheme.current.textPrimary
		private val accent = AppTheme.current.accent
		private val fill = Paint().apply { color = AppTheme.withAlpha(AppTheme.current.surface, 92) }
		private val grid = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			color = AppTheme.withAlpha(ink, 18)
			strokeWidth = dp(1).toFloat()
		}
		private val cross = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			color = AppTheme.withAlpha(accent, 60)
			strokeWidth = dp(1).toFloat()
		}
		private val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = accent }
		private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			color = accent
			style = Paint.Style.STROKE
			strokeWidth = dp(2).toFloat()
		}
		private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			color = AppTheme.withAlpha(ink, 60)
			textSize = dp(12).toFloat()
			typeface = AppFont.get(context)
		}

		override fun onDraw(canvas: Canvas) {
			val w = width.toFloat()
			val h = height.toFloat()
			canvas.drawRect(0f, 0f, w, h, fill)
			for (i in 1 until 8)
				canvas.drawLine(w * i / 8f, 0f, w * i / 8f, h, grid)
			for (i in 1 until 4)
				canvas.drawLine(0f, h * i / 4f, w, h * i / 4f, grid)
			canvas.drawText("XY PAD   two fingers", dp(12).toFloat(), dp(20).toFloat(), label)
			for (f in 0..1) {
				val down = fingers[f] >= 0
				// The second only while it is there.
				if (f == 1 && !down)
					continue
				val px = lastX[f] * w
				val py = (1f - lastY[f]) * h
				canvas.drawLine(px, 0f, px, h, cross)
				canvas.drawLine(0f, py, w, py, cross)
				// Filled while touched, hollow while holding the last position.
				if (down)
					canvas.drawCircle(px, py, dp(14).toFloat(), dot)
				else
					canvas.drawCircle(px, py, dp(10).toFloat(), ring)
			}
		}

		override fun onTouchEvent(event: MotionEvent): Boolean {
			val index = event.actionIndex
			val id = event.getPointerId(index)
			when (event.actionMasked) {
				MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
					val f = fingers.indexOf(-1)
					if (f >= 0)
						fingers[f] = id
				}
				MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP -> {
					val f = fingers.indexOf(id)
					if (f >= 0) {
						fingers[f] = -1
						onPad(f, lastX[f], lastY[f], false)
					}
				}
				MotionEvent.ACTION_CANCEL -> {
					fingers.fill(-1)
					lift()
				}
			}
			for (f in 0..1) {
				val p = if (fingers[f] >= 0) event.findPointerIndex(fingers[f]) else -1
				if (p < 0)
					continue
				lastX[f] = (event.getX(p) / width.coerceAtLeast(1)).coerceIn(0f, 1f)
				lastY[f] = (1f - event.getY(p) / height.coerceAtLeast(1)).coerceIn(0f, 1f)
				onPad(f, lastX[f], lastY[f], true)
			}
			invalidate()
			return true
		}
	}
}
