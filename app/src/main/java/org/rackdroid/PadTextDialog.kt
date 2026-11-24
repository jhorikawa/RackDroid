package org.rackdroid

import android.app.Activity
import android.app.AlertDialog
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.util.TypedValue
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.TextView
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat

/**
 * A window to type into with a controller: a text field, a keyboard for the
 * D-pad (PadKeyboard) and, for a search, the list of what was found between
 * them. A touch on the field brings up Android's own keyboard, which takes the
 * place of the one on screen for as long as it is there.
 *
 * Up from the top row of the keyboard goes to the list, down past its end
 * comes back.
 */
internal class PadTextDialog(
	private val activity: Activity,
	private val ink: Int,
	private val title: String,
	private val hint: String,
	private val doneLabel: String,
	/** What the buttons do, for the line at the bottom. */
	private val help: String,
	private val initial: String = "",
	/** The row of the list to begin on, or -1 to begin on the keyboard. */
	private val startRow: Int = -1,
) {
	/** The list's rows for a text; null for a window without a list. */
	var rowsFor: ((String) -> List<PadRow>)? = null
	/** A on a row of the list. */
	var onPick: ((Int) -> Unit)? = null
	/** Y on a row of the list. */
	var onY: ((Int) -> Unit)? = null
	/** The keyboard's last key, START, or Enter; with a list, they go to it. */
	var onDone: ((String) -> Unit)? = null
	/** B on the keyboard. */
	var onBack: (() -> Unit)? = null

	private val context = AlertDialog.Builder(activity).context
	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()
	private val faint = (ink and 0x00FFFFFF) or (0xB0 shl 24)

	private lateinit var dialog: AlertDialog
	private lateinit var input: EditText
	private lateinit var keyboard: PadKeyboard
	private var listView: ListView? = null
	private var empty: TextView? = null
	private var adapter: PadListAdapter? = null
	private var inList = false
	private var imeShown = false

	val text: String get() = input.text.toString()

	fun show(): AlertDialog {
		input = EditText(context).apply {
			hint = this@PadTextDialog.hint
			setText(initial)
			setSelection(initial.length)
			setTextColor(ink)
			setHintTextColor((ink and 0x00FFFFFF) or (0x80 shl 24))
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
			isSingleLine = true
			inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
			// Not full screen in landscape: the list is what is being typed for.
			imeOptions = EditorInfo.IME_ACTION_DONE or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
				EditorInfo.IME_FLAG_NO_FULLSCREEN
			setOnEditorActionListener { _, _, _ ->
				done()
				true
			}
			addTextChangedListener(object : TextWatcher {
				override fun afterTextChanged(s: Editable?) = fill()
				override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
				override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
			})
		}
		keyboard = PadKeyboard(context, ink, doneLabel, { type(it) }, { erase() }, { done() })
		val root = LinearLayout(context).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(16), dp(6), dp(16), dp(6))
			addView(LinearLayout(context).apply {
				orientation = LinearLayout.HORIZONTAL
				gravity = android.view.Gravity.CENTER_VERTICAL
				addView(TextView(context).apply {
					text = title
					setTextColor(ink)
					setTextSize(TypedValue.COMPLEX_UNIT_SP, 18f)
					setPadding(dp(8), 0, dp(12), 0)
				})
				addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
			})
		}
		if (rowsFor != null) {
			val lv = ListView(context).apply {
				divider = null
				selector = ColorDrawable(Color.TRANSPARENT)
				isFocusable = false
				setOnItemClickListener { _, _, i, _ -> pick(i) }
			}
			val none = TextView(context).apply {
				text = "Nothing matches"
				gravity = android.view.Gravity.CENTER
				setTextColor(faint)
				visibility = View.GONE
			}
			listView = lv
			empty = none
			root.addView(lv, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
			root.addView(none, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
		}
		root.addView(keyboard, LinearLayout.LayoutParams(
			ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { topMargin = dp(4) })
		val helpLine = TextView(context).apply {
			text = help
			setTextColor(faint)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
			gravity = android.view.Gravity.CENTER
			setPadding(0, dp(4), 0, 0)
		}
		root.addView(helpLine)
		// Asked for at each layout, which Android's keyboard coming and going
		// always causes: the insets themselves are spent on resizing the window
		// and never get as far as this view.
		root.viewTreeObserver.addOnGlobalLayoutListener {
			val shown = ViewCompat.getRootWindowInsets(root)
				?.isVisible(WindowInsetsCompat.Type.ime()) == true
			if (shown != imeShown) {
				imeShown = shown
				keyboard.visibility = if (shown) View.GONE else View.VISIBLE
				helpLine.visibility = keyboard.visibility
			}
		}

		dialog = AlertDialog.Builder(activity).setView(root).create()
		dialog.window?.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or
			WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)
		dialog.setOnKeyListener { _, code, event -> key(code, event) }
		dialog.show()
		dialog.window?.clearFlags(WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
			WindowManager.LayoutParams.FLAG_ALT_FOCUSABLE_IM)
		if (rowsFor != null)
			dialog.window?.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT)
		else
			dialog.window?.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
		input.requestFocus()
		fill()
		val lv = listView
		if (startRow >= 0 && lv != null) {
			adapter?.let { it.cursor = startRow.coerceIn(0, maxOf(0, it.count - 1)) }
			goTo(true)
			lv.setSelection(startRow)
		}
		return dialog
	}

	private fun fill() {
		val rows = rowsFor?.invoke(text) ?: return
		val lv = listView ?: return
		adapter = PadListAdapter(activity, context, rows, ink).also { it.lit = inList }
		lv.adapter = adapter
		lv.visibility = if (rows.isEmpty()) View.GONE else View.VISIBLE
		empty?.visibility = if (rows.isEmpty()) View.VISIBLE else View.GONE
		if (rows.isEmpty())
			goTo(false)
	}

	private fun type(s: String) {
		val at = input.selectionStart.coerceAtLeast(0)
		input.text.replace(at, input.selectionEnd.coerceAtLeast(at), s)
	}

	private fun erase() {
		val at = input.selectionStart.coerceAtLeast(0)
		val end = input.selectionEnd.coerceAtLeast(at)
		if (end > at) input.text.delete(at, end)
		else if (at > 0) input.text.delete(Character.offsetByCodePoints(input.text, at, -1), at)
	}

	private fun goTo(list: Boolean) {
		val go = list && (adapter?.count ?: 0) > 0
		inList = go
		keyboard.active = !go
		listView?.let { adapter?.light(it, go) }
	}

	private fun done() {
		if (listView != null) {
			hideIme()
			goTo(true)
			return
		}
		val t = text
		close()
		onDone?.invoke(t)
	}

	private fun pick(i: Int) {
		close()
		onPick?.invoke(i)
	}

	private fun close() {
		hideIme()
		dialog.dismiss()
	}

	private fun hideIme() {
		if (imeShown)
			(activity.getSystemService(Activity.INPUT_METHOD_SERVICE) as InputMethodManager)
				.hideSoftInputFromWindow(input.windowToken, 0)
	}

	/** A release only counts if this window saw the press: see GamepadMenu. */
	private val pressedHere = HashSet<Int>()

	private fun key(code: Int, event: KeyEvent): Boolean {
		val down = event.action == KeyEvent.ACTION_DOWN
		val first = down && event.repeatCount == 0
		val released = if (down) {
			if (first)
				pressedHere.add(code)
			false
		}
		else pressedHere.remove(code)
		val held = down && code in pressedHere
		when (code) {
			KeyEvent.KEYCODE_DPAD_UP -> if (down) vertical(-1)
			KeyEvent.KEYCODE_DPAD_DOWN -> if (down) vertical(1)
			KeyEvent.KEYCODE_DPAD_LEFT -> if (down && !inList) keyboard.move(-1, 0)
			KeyEvent.KEYCODE_DPAD_RIGHT -> if (down && !inList) keyboard.move(1, 0)
			KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> {
				// Typing answers the press. What leaves the window waits for
				// the release, so the next one does not open with A down.
				if (!inList) {
					if (first) {
						if (imeShown) hideIme() else keyboard.press()
					}
				}
				else if (released) {
					adapter?.let { pick(it.cursor) }
				}
			}
			KeyEvent.KEYCODE_BUTTON_Y -> {
				if (!inList) {
					if (held)
						erase()
				}
				else if (released && onY != null) {
					adapter?.let {
						close()
						onY?.invoke(it.cursor)
					}
				}
			}
			KeyEvent.KEYCODE_BUTTON_B -> if (released) {
				if (imeShown) hideIme()
				else if (inList) goTo(false)
				else {
					close()
					onBack?.invoke()
				}
			}
			KeyEvent.KEYCODE_BUTTON_START -> if (released) done()
			KeyEvent.KEYCODE_BUTTON_X -> if (released) goTo(!inList)
			KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1 -> if (down) {
				val lv = listView
				val a = adapter
				if (lv != null && a != null) {
					val page = maxOf(1, lv.lastVisiblePosition - lv.firstVisiblePosition)
					goTo(true)
					a.move(lv, a.cursor + if (code == KeyEvent.KEYCODE_BUTTON_L1) -page else page)
				}
			}
			else -> return false
		}
		return true
	}

	private fun vertical(d: Int) {
		val lv = listView
		val a = adapter
		if (!inList) {
			if (!keyboard.move(0, d))
				goTo(true)
			return
		}
		if (lv == null || a == null)
			return
		if (d > 0 && a.cursor >= a.count - 1)
			goTo(false)
		else
			a.move(lv, a.cursor + d)
	}
}
