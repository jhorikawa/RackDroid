package org.rackdroid

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import org.json.JSONArray
import org.json.JSONObject
import java.io.File

/** A picture of a made module's panel for the lists of modules, where the other
 * modules have one that came with them: drawn from what the module says of its
 * panel (nova_kit_impl.hpp, describe()). */
object MadeThumbnail {
	private const val HEIGHT = 380
	private const val SCALE = 1.5f

	fun file(filesDir: File, key: String) = File(filesDir, "thumbnails/$key.webp")

	fun write(filesDir: File, key: String, id: String, meta: JSONObject) {
		val panel = meta.optJSONObject("panel") ?: return
		val width = meta.optInt("width", 8) * 15
		if (width !in 15..1500)
			return
		val bitmap = Bitmap.createBitmap((width * SCALE).toInt(), (HEIGHT * SCALE).toInt(), Bitmap.Config.ARGB_8888)
		val canvas = Canvas(bitmap)
		canvas.scale(SCALE, SCALE)
		draw(canvas, width.toFloat(), panel, panel.optString("title").ifEmpty { id.uppercase() })
		val out = file(filesDir, key)
		out.parentFile?.mkdirs()
		@Suppress("DEPRECATION")
		out.outputStream().use { bitmap.compress(Bitmap.CompressFormat.WEBP, 85, it) }
		bitmap.recycle()
	}

	private fun color(text: String, otherwise: Int) = runCatching { Color.parseColor(text) }.getOrDefault(otherwise)

	private fun draw(canvas: Canvas, width: Float, panel: JSONObject, title: String) {
		val fill = Paint(Paint.ANTI_ALIAS_FLAG)
		val text = Paint(Paint.ANTI_ALIAS_FLAG).apply { textAlign = Paint.Align.CENTER }
		val ground = color(panel.optString("color"), 0xff1c2430.toInt())
		val light = Color.luminance(ground) > 0.35f
		val ink = if (light) 0xff14181e.toInt() else 0xfff2f5f8.toInt()
		canvas.drawColor(ground)
		fill.color = color(panel.optString("accent"), 0xff4fc3f7.toInt())
		canvas.drawRect(8f, 26f, width - 8f, 28f, fill)
		text.color = ink
		fit(canvas, text, title, width / 2, 20f, width - 12f, 15f)
		val displays = panel.optJSONArray("displays") ?: JSONArray()
		for (i in 0 until displays.length()) {
			val d = displays.getJSONArray(i)
			val x = d.getDouble(0).toFloat()
			val y = d.getDouble(1).toFloat()
			fill.color = 0xff0a0d12.toInt()
			canvas.drawRoundRect(x, y, x + d.getDouble(2).toFloat(), y + d.getDouble(3).toFloat(), 4f, 4f, fill)
		}
		val parts = panel.optJSONArray("parts") ?: JSONArray()
		for (i in 0 until parts.length()) {
			val p = parts.getJSONArray(i)
			val kind = p.getString(0)
			val x = p.getDouble(1).toFloat()
			val y = p.getDouble(2).toFloat()
			val r = p.getDouble(3).toFloat()
			if (kind == "out") {
				fill.color = if (light) 0xc8000000.toInt() else 0xe0ffffff.toInt()
				canvas.drawRoundRect(x - 17f, y - r - 13f, x + 17f, y + r + 3f, 4f, 4f, fill)
			}
			when (kind) {
				"in", "out" -> {
					fill.color = 0xff8a857c.toInt()
					canvas.drawCircle(x, y, r, fill)
					fill.color = 0xff0c0c0c.toInt()
					canvas.drawCircle(x, y, r * 0.55f, fill)
				}
				"light" -> {
					fill.color = 0xff7bd88f.toInt()
					canvas.drawCircle(x, y, r, fill)
				}
				"switch" -> {
					fill.color = 0xff0c0c0c.toInt()
					canvas.drawRoundRect(x - 7f, y - r, x + 7f, y + r, 3f, 3f, fill)
					fill.color = 0xffd0cbc0.toInt()
					canvas.drawRoundRect(x - 5f, y, x + 5f, y + r - 2f, 2f, 2f, fill)
				}
				"knob", "button" -> {
					fill.color = 0xff191919.toInt()
					canvas.drawCircle(x, y, r, fill)
					fill.color = 0xff3a3733.toInt()
					canvas.drawCircle(x, y, r * 0.82f, fill)
					if (kind == "knob") {
						fill.color = 0xfff0e6d0.toInt()
						canvas.drawRect(x - 1.2f, y - r * 0.8f, x + 1.2f, y - r * 0.2f, fill)
					}
				}
			}
			text.color = if (kind == "out") (if (light) Color.WHITE else 0xff14181e.toInt()) else ink
			fit(canvas, text, p.getString(4), x, y - r - 3f, if (kind == "out") 32f else 60f, 10.5f)
		}
	}

	private fun fit(canvas: Canvas, paint: Paint, s: String, x: Float, y: Float, width: Float, size: Float) {
		paint.textSize = size
		val w = paint.measureText(s)
		if (w > width)
			paint.textSize = maxOf(size * width / w, size * 0.6f)
		canvas.drawText(s, x, y, paint)
	}
}
