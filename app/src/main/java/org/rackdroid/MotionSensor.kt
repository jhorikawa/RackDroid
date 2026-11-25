package org.rackdroid

import android.app.Activity
import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import kotlin.math.exp
import kotlin.math.sqrt

/** How the handheld is moved, for play mode (gamepad_midi.cpp): turned like a
 * steering wheel (CC 26, right is positive) and shaken (CC 27, and 28 as a gate).
 * Where it is held does not matter, unlike the tilt (TiltSensor). */
class MotionSensor(activity: Activity, private val onMotion: (Float, Float, Boolean) -> Unit) : SensorEventListener {
	private companion object {
		/** Radians a second at full. */
		const val TWIST_FULL = 5f
		const val TWIST_REST = 0.06f
		/** Metres a second squared, gravity taken off. */
		const val SHAKE_FULL = 20f
		const val SHAKE_REST = 1.2f
		const val SHAKEN_ON = 0.45f
		const val SHAKEN_OFF = 0.25f
		/** How long a shake rings, in seconds. */
		const val RING = 0.12f
	}

	private val manager = activity.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
	private val gyro = manager?.getDefaultSensor(Sensor.TYPE_GYROSCOPE)
	private val linear = manager?.getDefaultSensor(Sensor.TYPE_LINEAR_ACCELERATION)
	private var wanted = false
	private var listening = false
	private var twist = 0f
	private var shake = 0f
	private var shaken = false
	private var at = 0L

	fun setWanted(on: Boolean) {
		wanted = on
		if (on) resume() else pause()
	}

	fun resume() {
		if (!wanted || listening)
			return
		at = 0L
		for (s in listOfNotNull(gyro, linear))
			listening = manager?.registerListener(this, s, SensorManager.SENSOR_DELAY_GAME) == true || listening
	}

	fun pause() {
		if (!listening)
			return
		manager?.unregisterListener(this)
		listening = false
		twist = 0f
		shake = 0f
		shaken = false
		onMotion(0f, 0f, false)
	}

	override fun onSensorChanged(event: SensorEvent) {
		val v = event.values
		if (event.sensor.type == Sensor.TYPE_GYROSCOPE) {
			// Round the axis out of the screen, whichever way that is turned:
			// against the clock is positive there.
			val z = -v[2]
			val a = kotlin.math.abs(z)
			val to = if (a < TWIST_REST) 0f else (z / TWIST_FULL).coerceIn(-1f, 1f)
			twist += (to - twist) * 0.4f
			if (to == 0f && kotlin.math.abs(twist) < 0.004f)
				twist = 0f
		} else {
			val g = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
			val dt = if (at == 0L) 0.02f else ((event.timestamp - at) * 1e-9f).coerceIn(0f, 0.2f)
			at = event.timestamp
			val hit = ((g - SHAKE_REST) / (SHAKE_FULL - SHAKE_REST)).coerceIn(0f, 1f)
			shake = maxOf(hit, shake * exp(-dt / RING))
			if (shake < 0.004f)
				shake = 0f
			shaken = if (shaken) shake > SHAKEN_OFF else shake > SHAKEN_ON
		}
		onMotion(twist, shake, shaken)
	}

	override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
}
