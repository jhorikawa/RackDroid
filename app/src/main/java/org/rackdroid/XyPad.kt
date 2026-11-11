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
	private val onPad: (Float, Float, Boolean) -> Unit,
) {
	private var popup: PopupWindow? = null

	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	val showing get() = popup != null

	fun toggle() {
		popup?.let {
			close()
			return
		}
		val pad = PadView(activity)
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
		val p = PopupWindow(holder, ViewGroup.LayoutParams.MATCH_PARENT, dp(200))
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
		// A finger still down when the pad goes away never sends its lift.
		onPad(lastX, lastY, false)
	}

	/** The app left the screen with a finger possibly still on the pad. */
	fun release() {
		if (popup != null)
			onPad(lastX, lastY, false)
	}

	private var lastX = 0f
	private var lastY = 0f

	private inner class PadView(context: Context) : View(context) {
		private var down = false
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
			canvas.drawText("XY PAD   X = left to right   Y = bottom to top   touch = gate", dp(12).toFloat(), dp(20).toFloat(), label)
			val px = lastX * w
			val py = (1f - lastY) * h
			canvas.drawLine(px, 0f, px, h, cross)
			canvas.drawLine(0f, py, w, py, cross)
			// Filled while touched, hollow while holding the last position.
			if (down)
				canvas.drawCircle(px, py, dp(14).toFloat(), dot)
			else
				canvas.drawCircle(px, py, dp(10).toFloat(), ring)
		}

		override fun onTouchEvent(event: MotionEvent): Boolean {
			when (event.actionMasked) {
				MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> {
					down = true
					lastX = (event.x / width.coerceAtLeast(1)).coerceIn(0f, 1f)
					lastY = (1f - event.y / height.coerceAtLeast(1)).coerceIn(0f, 1f)
				}
				MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> down = false
				else -> return true
			}
			onPad(lastX, lastY, down)
			invalidate()
			return true
		}
	}
}
