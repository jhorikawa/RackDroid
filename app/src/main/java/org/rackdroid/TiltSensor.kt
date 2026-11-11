package org.rackdroid

import android.app.Activity
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.Surface
import kotlin.math.asin
import kotlin.math.atan2
import kotlin.math.sqrt

/**
 * The handheld's tilt as two control voltages (gamepad_midi.cpp sends them as
 * CC 21 and 22 of the "Gamepad" MIDI driver).
 *
 * Tilt is measured from how the device is being held when the sensor starts,
 * not from level: nobody holds a handheld flat, and a control that only rests
 * at zero in a pose nobody uses is no control. Starting it again -- leaving and
 * re-entering play mode -- takes the pose of that moment as the new centre.
 *
 *   x  right edge down is positive
 *   y  top edge away from you is positive (a stick pushed forward)
 *
 * Both reach +-1 at RANGE_DEGREES either way.
 *
 * Lives on the Kotlin side because the answer depends on how the screen is
 * turned relative to the sensor's axes (this handheld's panel is mounted a
 * quarter turn from the sensor), and the display's rotation is one call away
 * here.
 */
class TiltSensor(private val activity: Activity, private val onTilt: (Float, Float) -> Unit) : SensorEventListener {
	companion object {
		private const val RANGE_DEGREES = 35.0
		/** Share of the gap to the new reading closed per sample: enough to
		 * take the hand's tremor out without the voltage lagging the wrist. */
		private const val SMOOTHING = 0.25f
	}

	private val manager = activity.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
	// The fused gravity sensor where there is one: an accelerometer alone
	// also reports every shake as tilt.
	private val sensor: Sensor? = manager?.getDefaultSensor(Sensor.TYPE_GRAVITY)
		?: manager?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)

	/** Asked for (play mode is on). */
	private var wanted = false
	/** Actually listening: wanted, and the app is in front. */
	private var listening = false
	private var haveCentre = false
	private var centreRoll = 0.0
	private var centrePitch = 0.0
	private var x = 0f
	private var y = 0f

	val available get() = sensor != null

	fun setWanted(on: Boolean) {
		wanted = on
		if (on) listen() else unlisten()
	}

	/** The app left the screen: a sensor kept running there only drains the
	 * battery, and its tilt would be playing a patch nobody is holding. */
	fun pause() = unlisten()

	fun resume() {
		if (wanted)
			listen()
	}

	private fun listen() {
		if (listening || sensor == null)
			return
		haveCentre = false
		listening = manager?.registerListener(this, sensor, SensorManager.SENSOR_DELAY_GAME) == true
	}

	private fun unlisten() {
		if (!listening)
			return
		manager?.unregisterListener(this)
		listening = false
		x = 0f
		y = 0f
		onTilt(0f, 0f)
	}

	override fun onSensorChanged(event: SensorEvent) {
		// The sensor's axes belong to the device's natural orientation; turn
		// them into the screen's (x to the right, y up, as it is being looked
		// at). Same table as Android's own accelerometer sample.
		@Suppress("DEPRECATION")
		val rotation = activity.windowManager.defaultDisplay.rotation
		val v = event.values
		val sx: Float
		val sy: Float
		when (rotation) {
			Surface.ROTATION_90 -> { sx = -v[1]; sy = v[0] }
			Surface.ROTATION_180 -> { sx = -v[0]; sy = -v[1] }
			Surface.ROTATION_270 -> { sx = v[1]; sy = -v[0] }
			else -> { sx = v[0]; sy = v[1] }
		}
		val sz = v[2]
		val g = sqrt(sx * sx + sy * sy + sz * sz)
		if (g < 1f)
			return
		// The reading points away from the ground. Right edge down leans it
		// toward the left edge: a negative x. Flat on a table the pitch is 0,
		// held upright it is a quarter turn.
		val roll = asin((sx / g).toDouble().coerceIn(-1.0, 1.0))
		val pitch = atan2(sy.toDouble(), sz.toDouble())
		if (!haveCentre) {
			centreRoll = roll
			centrePitch = pitch
			haveCentre = true
		}
		val range = Math.toRadians(RANGE_DEGREES)
		var dPitch = pitch - centrePitch
		// Past upside down the angle wraps; take the short way round.
		if (dPitch > Math.PI) dPitch -= 2 * Math.PI
		if (dPitch < -Math.PI) dPitch += 2 * Math.PI
		val tx = (-(roll - centreRoll) / range).coerceIn(-1.0, 1.0).toFloat()
		val ty = (-dPitch / range).coerceIn(-1.0, 1.0).toFloat()
		x += (tx - x) * SMOOTHING
		y += (ty - y) * SMOOTHING
		onTilt(x, y)
	}

	override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
