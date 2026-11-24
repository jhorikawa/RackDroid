package org.rackdroid

import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.text.InputType
import android.util.TypedValue
import android.view.Gravity
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.view.inputmethod.EditorInfo
import android.view.inputmethod.InputMethodManager
import android.widget.EditText
import android.widget.ImageButton
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import org.json.JSONArray
import org.json.JSONObject

/**
 * The chat: a panel along the right edge, with the rack to the left of it so
 * that the patch can be watched as it changes. A finger still reaches the rack;
 * the controller belongs to the panel while it is up, in one of two states.
 *
 *   typing    Android's keyboard, with its Japanese and its microphone; or, if
 *             that is chosen, one for the D-pad (PadKeyboard)
 *   reading   D-pad up and down scroll, Y stops the answer or takes back the
 *             last one, X chooses the model and the effort, A goes to typing
 *
 * B puts the panel away; what is being answered goes on. The conversation lasts
 * as long as the app does.
 */
class ChatPanel(
	private val activity: Activity,
	private val server: PatchServer,
	private val bridge: TermuxBridge,
	private val settings: ChatSettings,
	private val menu: GamepadMenu,
	private val togglePause: () -> Unit,
) {
	private companion object {
		const val WIDTH_DP = 300
		/** How long Claude Code may take to come up. */
		const val START_MS = 25000L
		const val MAX_MESSAGES = 60
	}

	private enum class Who { USER, CLAUDE, NOTE }

	private class Message(val who: Who, var text: String) {
		/** What was done to the patch, as it is done. */
		val added = ArrayList<String>()
		var removed = 0
		var cables = 0
		var unplugged = 0
		var knobs = 0
		var choices = 0
		var actions = 0
		var moved = 0
		val files = ArrayList<String>()
		var listened = false
		var undid = false
		var view: TextView? = null

		fun work(): String {
			val parts = ArrayList<String>()
			if (added.isNotEmpty()) parts.add(added.joinToString(" ") { "+$it" })
			if (removed > 0) parts.add("−$removed module${if (removed > 1) "s" else ""}")
			if (cables > 0) parts.add("$cables cable${if (cables > 1) "s" else ""}")
			if (unplugged > 0) parts.add("$unplugged unplugged")
			if (knobs > 0) parts.add("$knobs knob${if (knobs > 1) "s" else ""}")
			if (choices > 0) parts.add("$choices setting${if (choices > 1) "s" else ""}")
			if (moved > 0) parts.add("$moved moved")
			if (actions > 0) parts.add("$actions action${if (actions > 1) "s" else ""}")
			parts.addAll(files)
			if (undid) parts.add("undo")
			if (listened) parts.add("measured")
			return parts.joinToString("  ·  ")
		}
	}

	private val ui = Handler(Looper.getMainLooper())
	private val messages = ArrayList<Message>()
	private var answer: Message? = null
	private var askedAt = 0L
	private var stopping = false
	/** Typed before there was anyone to answer it. */
	private var unsent: String? = null
	private var startedAt = 0L
	private var draft = ""

	private var dialog: Dialog? = null
	private lateinit var list: LinearLayout
	private lateinit var scroll: ScrollView
	private lateinit var status: TextView
	private lateinit var input: EditText
	private lateinit var send: ImageButton
	private lateinit var keyboard: PadKeyboard
	private lateinit var help: TextView
	private var typing = true
	private var imeShown = false

	private val ink get() = AppTheme.current.textPrimary
	private val faint get() = AppTheme.current.textSecondary
	private fun dp(v: Int) = (v * activity.resources.displayMetrics.density).toInt()

	val showing get() = dialog?.isShowing == true
	private val working get() = answer != null

	init {
		server.onEvent = { o -> ui.post { onEvent(o) } }
	}

	fun toggle() {
		if (showing) close() else open()
	}

	/** Termux's permission was answered. */
	fun permissionAnswered(granted: Boolean) {
		if (granted)
			connect()
		else
			fail("Without that permission RackDroid cannot start Claude Code in Termux.")
	}

	/* ---- Getting someone to answer ---- */

	private fun connect() {
		if (!server.start()) {
			fail("This build cannot open the chat link.")
			return
		}
		if (server.bridgeConnected) {
			startedAt = 0L
			refreshStatus()
			return
		}
		when (bridge.start()) {
			TermuxBridge.Outcome.STARTED -> {
				val mine = SystemClock.uptimeMillis()
				startedAt = mine
				refreshStatus()
				ui.postDelayed({
					if (startedAt == mine && !server.bridgeConnected) {
						startedAt = 0L
						fail(TermuxBridge.SETUP)
					}
				}, START_MS)
			}
			TermuxBridge.Outcome.NO_TERMUX -> fail("The chat is answered by Claude Code in Termux, and Termux is not installed.")
			TermuxBridge.Outcome.ASKED -> {}
			TermuxBridge.Outcome.FAILED -> fail(TermuxBridge.SETUP)
		}
	}

	/** There is nobody to answer, and what has been asked is not left waiting. */
	private fun fail(text: String) {
		unsent = null
		finish(text)
		refreshStatus()
	}

	private fun onEvent(o: JSONObject) {
		when (o.optString("event")) {
			"bridge" -> {
				startedAt = 0L
				if (o.optBoolean("connected"))
					unsent?.let {
						unsent = null
						server.toBridge(JSONObject().put("chat", it))
					}
				else if (working)
					finish("Claude Code went away.")
			}
			"text" -> answer?.let {
				it.text += o.optString("text")
				show(it)
			}
			"tool" -> answer?.let {
				count(it, o.optString("name"), o.optJSONObject("args") ?: JSONObject())
				show(it)
			}
			"done" -> {
				val m = answer
				if (m != null && o.optBoolean("stopped"))
					m.text = (m.text.trim() + "\nStopped.").trim()
				else if (m != null && !o.optBoolean("ok"))
					m.text = o.optString("text").ifBlank { "That did not work." }
				else if (m != null && m.text.isBlank())
					m.text = o.optString("text")
				finish(null)
			}
			"error" -> finish(o.optString("text"))
		}
		refreshStatus()
	}

	private fun count(m: Message, tool: String, args: JSONObject) {
		fun size(key: String) = (args.optJSONArray(key) ?: JSONArray()).length()
		when (tool) {
			"add_modules" -> {
				val a = args.optJSONArray("models") ?: JSONArray()
				for (i in 0 until a.length())
					m.added.add(a.optString(i).substringAfter('/'))
			}
			"remove_modules" -> m.removed += size("modules")
			"connect" -> m.cables += size("cables")
			"disconnect" -> m.unplugged += size("cables")
			"set_knobs" -> m.knobs += size("knobs")
			"set_menu" -> m.choices += size("choices")
			"module_action" -> m.actions += size("modules")
			"move_modules" -> m.moved += size("moves")
			"save_patch" -> m.files.add("save")
			"open_patch" -> m.files.add("open")
			"play" -> m.files.add(if (args.optBoolean("on")) "play" else "pause")
			"listen" -> m.listened = true
			"undo" -> m.undid = true
		}
	}

	private fun finish(problem: String?) {
		val m = answer
		answer = null
		stopping = false
		if (m != null) {
			if (problem != null)
				m.text = (m.text.trim() + "\n" + problem).trim()
			show(m)
		} else if (problem != null)
			note(problem)
		if (!showing && m != null)
			Toast.makeText(activity, m.text.take(140), Toast.LENGTH_LONG).show()
	}

	private fun ask() {
		val text = input.text.toString().trim()
		if (text.isEmpty() || working)
			return
		hideIme()
		input.setText("")
		draft = ""
		append(Message(Who.USER, text))
		val m = Message(Who.CLAUDE, "")
		answer = m
		askedAt = SystemClock.uptimeMillis()
		append(m)
		go(false)
		if (!server.toBridge(JSONObject().put("chat", text))) {
			unsent = text
			connect()
		}
		refreshStatus()
		ui.postDelayed(clock, 1000)
	}

	private val clock = object : Runnable {
		override fun run() {
			if (!working)
				return
			refreshStatus()
			ui.postDelayed(this, 1000)
		}
	}

	private fun stopAnswer() {
		if (!working || stopping)
			return
		stopping = true
		if (unsent != null) {
			unsent = null
			finish("Stopped.")
		} else
			server.toBridge(JSONObject().put("stop", true))
		refreshStatus()
	}

	private fun undoLast() {
		Thread {
			val problem = server.undo()
			ui.post { note(problem ?: "The last answer's changes are undone.") }
		}.start()
	}

	private fun showSettings() {
		hideIme()
		val rows = listOf(
			PadRow("Model: ${settings.modelName}", note = "What answers"),
			PadRow("Effort: ${settings.effort}", note = "How long it thinks about each answer"),
			PadRow("Keyboard: ${if (settings.padKeyboard) "D-pad" else "Android"}", note = "What you type with"))
		menu.choose("Chat", rows, 0) {
			when (it) {
				0 -> showModels()
				1 -> showEfforts()
				else -> showKeyboards()
			}
		}
	}

	private fun showKeyboards() {
		val pad = settings.padKeyboard
		menu.choose("Keyboard", listOf(
			menu.choiceRow("Android", !pad, "Touch, with Japanese and the microphone"),
			menu.choiceRow("D-pad", pad, "Letters and digits, without touching the screen")), if (pad) 1 else 0) {
			settings.padKeyboard = it == 1
			go(typing)
		}
	}

	private fun showModels() {
		val all = ChatSettings.MODELS
		menu.choose("Model", all.map { menu.choiceRow(it.name, it.id == settings.model, it.note) },
			all.indexOfFirst { it.id == settings.model }) { set(all[it].id, settings.effort) }
	}

	private fun showEfforts() {
		val all = ChatSettings.EFFORTS
		menu.choose("Effort", all.map { menu.choiceRow(it.id, it.id == settings.effort, it.note.ifEmpty { null }) },
			all.indexOfFirst { it.id == settings.effort }) { set(settings.model, all[it].id) }
	}

	private fun set(model: String, effort: String) {
		if (model == settings.model && effort == settings.effort)
			return
		settings.set(model, effort)
		server.toBridge(JSONObject().put("settings", settings.json()))
		note("From the next message: ${settings.modelName}, effort ${settings.effort}.")
	}

	private fun newChat() {
		if (working)
			return
		messages.clear()
		if (::list.isInitialized)
			list.removeAllViews()
		server.toBridge(JSONObject().put("new", true))
		greet()
		go(true)
	}

	/* ---- The panel ---- */

	private fun bubble(m: Message): TextView = TextView(activity).apply {
		setTextColor(if (m.who == Who.USER) AppTheme.current.onAccent else if (m.who == Who.NOTE) faint else ink)
		setTextSize(TypedValue.COMPLEX_UNIT_SP, if (m.who == Who.NOTE) 12f else 14f)
		setLineSpacing(0f, 1.1f)
		setPadding(dp(10), dp(7), dp(10), dp(7))
		setTextIsSelectable(false)
		if (m.who != Who.NOTE)
			background = GradientDrawable().apply {
				cornerRadius = dp(12).toFloat()
				setColor(if (m.who == Who.USER) AppTheme.current.accent else AppTheme.current.surfaceInset)
			}
	}

	private fun add(m: Message) {
		val v = bubble(m)
		m.view = v
		list.addView(v, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
			gravity = if (m.who == Who.USER) Gravity.END else Gravity.START
			setMargins(if (m.who == Who.USER) dp(36) else 0, dp(4), 0, dp(4))
		})
		show(m)
	}

	private fun show(m: Message) {
		val v = m.view ?: return
		val work = m.work()
		val text = m.text.trim()
		v.text = when {
			work.isEmpty() && text.isEmpty() -> "…"
			work.isEmpty() -> text
			else -> android.text.SpannableStringBuilder().apply {
				append(work)
				setSpan(android.text.style.ForegroundColorSpan(AppTheme.current.accent), 0, work.length, 0)
				setSpan(android.text.style.RelativeSizeSpan(0.86f), 0, work.length, 0)
				if (text.isNotEmpty())
					append("\n").append(text)
			}
		}
		toEnd()
	}

	private fun toEnd() {
		if (::scroll.isInitialized)
			scroll.post { scroll.smoothScrollTo(0, list.height) }
	}

	private fun append(m: Message) {
		messages.add(m)
		while (messages.size > MAX_MESSAGES) {
			messages.removeAt(0)
			if (showing)
				list.removeViewAt(0)
		}
		if (showing)
			add(m)
	}

	private fun note(text: String) {
		append(Message(Who.NOTE, text))
		refreshStatus()
	}

	private fun greet() {
		if (messages.isEmpty())
			note("Say what the patch should do. Claude adds modules, patches cables and turns knobs. Each answer is one step of Undo.")
	}

	private fun refreshStatus() {
		if (!showing)
			return
		status.text = when {
			stopping -> "Stopping…"
			working && unsent != null -> "Starting Claude Code…"
			working -> "Working… ${(SystemClock.uptimeMillis() - askedAt) / 1000} s"
			server.bridgeConnected -> "${settings.modelName} · ${settings.effort} ▾"
			startedAt > 0L -> "Starting Claude Code…"
			else -> "Not connected"
		}
		send.setImageResource(if (working) R.drawable.ic_tb_close else R.drawable.ic_chat_send)
		send.contentDescription = if (working) "Stop" else "Send"
		help.text = when {
			typing -> "A type   Y erase   START send   X read   B close"
			working -> "↑↓ scroll   X model   Y stop   B close, it goes on"
			else -> "A type   X model   Y undo the answer   B close"
		}
	}

	private fun go(toTyping: Boolean) {
		typing = toTyping
		if (!showing)
			return
		showPadKeyboard()
		if (!settings.padKeyboard) {
			if (typing) showIme() else hideIme()
		}
		refreshStatus()
		toEnd()
	}

	private fun showPadKeyboard() {
		keyboard.visibility = if (typing && settings.padKeyboard && !imeShown) View.VISIBLE else View.GONE
	}

	private fun icon(res: Int, what: String, onClick: () -> Unit) = ImageButton(activity).apply {
		setImageResource(res)
		imageTintList = android.content.res.ColorStateList.valueOf(ink)
		scaleType = ImageView.ScaleType.FIT_CENTER
		contentDescription = what
		background = null
		isFocusable = false
		setPadding(dp(8), dp(8), dp(8), dp(8))
		setOnClickListener { onClick() }
	}

	private fun open() {
		val context: Context = activity
		list = LinearLayout(context).apply {
			orientation = LinearLayout.VERTICAL
			setPadding(dp(10), dp(4), dp(10), dp(4))
		}
		scroll = ScrollView(context).apply {
			isFocusable = false
			isFillViewport = false
			addView(list)
		}
		status = TextView(context).apply {
			setTextColor(faint)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f)
			setPadding(0, dp(8), 0, dp(8))
			setOnClickListener { showSettings() }
		}
		input = EditText(context).apply {
			setText(draft)
			setSelection(draft.length)
			hint = "Ask for a change"
			setTextColor(ink)
			setHintTextColor(AppTheme.current.textDisabled)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
			maxLines = 3
			background = GradientDrawable().apply {
				cornerRadius = dp(12).toFloat()
				setColor(AppTheme.current.surfaceInset)
			}
			setPadding(dp(12), dp(8), dp(12), dp(8))
			inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_MULTI_LINE or
				InputType.TYPE_TEXT_FLAG_CAP_SENTENCES
			imeOptions = EditorInfo.IME_ACTION_SEND or EditorInfo.IME_FLAG_NO_EXTRACT_UI or
				EditorInfo.IME_FLAG_NO_FULLSCREEN
			// A multi-line field that still sends with the keyboard's key.
			setRawInputType(InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_SENTENCES)
			setOnEditorActionListener { _, _, _ ->
				ask()
				true
			}
			setOnClickListener { go(true) }
		}
		send = ImageButton(context).apply {
			imageTintList = android.content.res.ColorStateList.valueOf(AppTheme.current.onAccent)
			scaleType = ImageView.ScaleType.FIT_CENTER
			setPadding(dp(9), dp(9), dp(9), dp(9))
			isFocusable = false
			background = GradientDrawable().apply {
				shape = GradientDrawable.OVAL
				setColor(AppTheme.current.accent)
			}
			setOnClickListener { if (working) stopAnswer() else ask() }
		}
		keyboard = PadKeyboard(context, ink, "send", { type(it) }, { erase() }, { ask() })
		help = TextView(context).apply {
			setTextColor(faint)
			setTextSize(TypedValue.COMPLEX_UNIT_SP, 11f)
			gravity = Gravity.CENTER
			setPadding(0, dp(3), 0, dp(6))
		}
		val header = LinearLayout(context).apply {
			orientation = LinearLayout.HORIZONTAL
			gravity = Gravity.CENTER_VERTICAL
			setPadding(dp(14), dp(4), dp(4), 0)
			addView(TextView(context).apply {
				text = "Chat"
				setTextColor(ink)
				setTypeface(AppFont.get(context), Typeface.BOLD)
				setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f)
				setPadding(0, 0, dp(10), 0)
			})
			addView(status, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
			addView(icon(R.drawable.ic_tb_undo, "Undo the last answer") { undoLast() }, LinearLayout.LayoutParams(dp(38), dp(38)))
			addView(icon(R.drawable.ic_chat_new, "New chat") { newChat() }, LinearLayout.LayoutParams(dp(38), dp(38)))
			addView(icon(R.drawable.ic_tb_close, "Close") { close() }, LinearLayout.LayoutParams(dp(38), dp(38)))
		}
		val root = LinearLayout(context).apply {
			orientation = LinearLayout.VERTICAL
			setBackgroundColor(AppTheme.withAlpha(AppTheme.current.surface, 96))
			addView(header)
			addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
			addView(LinearLayout(context).apply {
				orientation = LinearLayout.HORIZONTAL
				gravity = Gravity.BOTTOM
				setPadding(dp(8), dp(4), dp(8), dp(4))
				addView(input, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
				addView(send, LinearLayout.LayoutParams(dp(40), dp(40)).apply { leftMargin = dp(6) })
			})
			addView(keyboard, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
				setMargins(dp(4), 0, dp(4), 0)
			})
			addView(help)
		}
		root.viewTreeObserver.addOnGlobalLayoutListener {
			val shown = ViewCompat.getRootWindowInsets(root)?.isVisible(WindowInsetsCompat.Type.ime()) == true
			if (shown != imeShown) {
				imeShown = shown
				// Put away by its own key or a swipe: back to reading. Come up
				// by a touch on the field: typing.
				if (!settings.padKeyboard)
					typing = shown
				showPadKeyboard()
				refreshStatus()
				help.visibility = if (shown) View.GONE else View.VISIBLE
				toEnd()
			}
		}

		val d = Dialog(activity)
		dialog = d
		d.requestWindowFeature(Window.FEATURE_NO_TITLE)
		d.setContentView(root)
		d.setCanceledOnTouchOutside(false)
		d.setOnKeyListener { _, code, event -> key(code, event) }
		d.setOnDismissListener {
			draft = input.text.toString()
			dialog = null
			imeShown = false
			nativeChatInset(0)
		}
		d.window?.apply {
			setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
			setGravity(Gravity.END or Gravity.TOP)
			clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
			// The rack beside it stays the finger's.
			addFlags(WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN)
			setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE or
				WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_HIDDEN)
			@Suppress("DEPRECATION")
			decorView.systemUiVisibility = activity.window.decorView.systemUiVisibility
		}
		d.show()
		d.window?.setLayout(dp(WIDTH_DP), ViewGroup.LayoutParams.MATCH_PARENT)
		nativeChatInset(dp(WIDTH_DP))
		messages.forEach { add(it) }
		greet()
		input.requestFocus()
		connect()
		go(!working)
		// Asked for before the window has the focus, it does not come.
		if (typing && !settings.padKeyboard)
			input.postDelayed({ if (showing && typing) showIme() }, 250)
	}

	fun close() {
		hideIme()
		dialog?.dismiss()
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

	private fun showIme() {
		val w = dialog?.window ?: return
		input.requestFocus()
		androidx.core.view.WindowCompat.getInsetsController(w, input).show(WindowInsetsCompat.Type.ime())
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
		} else pressedHere.remove(code)
		val held = down && code in pressedHere
		// With Android's keyboard up, the D-pad moves in the text.
		if (imeShown && code in KeyEvent.KEYCODE_DPAD_UP..KeyEvent.KEYCODE_DPAD_CENTER)
			return false
		when (code) {
			KeyEvent.KEYCODE_DPAD_UP -> if (down) {
				if (typing && settings.padKeyboard) keyboard.move(0, -1) else scroll.smoothScrollBy(0, -dp(60))
			}
			KeyEvent.KEYCODE_DPAD_DOWN -> if (down) {
				if (typing && settings.padKeyboard) keyboard.move(0, 1) else scroll.smoothScrollBy(0, dp(60))
			}
			KeyEvent.KEYCODE_DPAD_LEFT -> if (down && typing) keyboard.move(-1, 0)
			KeyEvent.KEYCODE_DPAD_RIGHT -> if (down && typing) keyboard.move(1, 0)
			KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_DPAD_CENTER -> {
				if (typing && settings.padKeyboard) {
					if (first) {
						if (imeShown) hideIme() else keyboard.press()
					}
				} else if (!imeShown && released && !working)
					go(true)
			}
			KeyEvent.KEYCODE_BUTTON_Y -> {
				if (typing) {
					if (held)
						erase()
				} else if (released) {
					if (working) stopAnswer() else undoLast()
				}
			}
			KeyEvent.KEYCODE_BUTTON_X -> if (released) {
				if (typing) go(false) else showSettings()
			}
			KeyEvent.KEYCODE_BUTTON_B -> if (released) {
				if (imeShown) hideIme() else close()
			}
			KeyEvent.KEYCODE_BUTTON_START -> if (released) {
				if (typing) ask() else if (!working) go(true)
			}
			KeyEvent.KEYCODE_BUTTON_SELECT -> if (released) togglePause()
			KeyEvent.KEYCODE_BUTTON_L1 -> if (down) scroll.smoothScrollBy(0, -scroll.height * 3 / 4)
			KeyEvent.KEYCODE_BUTTON_R1 -> if (down) scroll.smoothScrollBy(0, scroll.height * 3 / 4)
			KeyEvent.KEYCODE_BACK -> if (released) {
				if (imeShown) hideIme() else close()
			}
			else -> return false
		}
		return true
	}

	private external fun nativeChatInset(px: Int)
}
