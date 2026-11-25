#!/usr/bin/env python3
"""Between RackDroid's chat and Claude Code, on the same device.

    nova_bridge.py run            answer what is typed in RackDroid's chat
    nova_bridge.py say TEXT...    type something there from a shell
    nova_bridge.py op JSON...     one operation on the patch, for trying things
    nova_bridge.py mcp            Claude Code starts this one: the patch as tools

Claude Code is run as it was published and as it was signed in by whoever owns
the device; nothing here sees or handles that. It is given the tools below and
no others: no files, no shell, no web. Making a module is one of them: it writes
the source, and nova_build.py compiles it, tries it out and hands it over.

RackDroid starts this itself, in Termux, when its chat is opened (TermuxBridge.kt)
and hands it the port and the token. To try things from elsewhere, copy the app's
chat-link.json to ~/.nova-chat-link.json, or name it in NOVA_CHAT_LINK.

Python 3.9 or later, and nothing else.
"""
import base64
import hashlib
import hmac
import importlib.util
import json
import os
import queue
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
import zipfile

CONFIG = os.environ.get("NOVA_CHAT_LINK") or os.path.expanduser("~/.nova-chat-link.json")
CLAUDE = os.environ.get("NOVA_CLAUDE", "claude")
# What answers, unless RackDroid's chat says otherwise (START > Chat).
MODEL = os.environ.get("NOVA_CHAT_MODEL", "opus")
EFFORT = os.environ.get("NOVA_CHAT_EFFORT", "low")
EFFORTS = ("low", "medium", "high", "xhigh", "max")
WORK = os.path.expanduser("~/.nova-chat")
# Where modules are made. Under /data, or Android's linker does not know its way
# to the libraries of the system; and not where something else is bound over it.
MAKE = os.environ.get("NOVA_MAKE", "/data/data/com.termux/files/home/nova-modules")

PROMPT = """\
You are the assistant inside RackDroid, VCV Rack on a small handheld. The user \
types to you while looking at the patch on the screen, and you change the patch \
with your tools. You see the patch only through them, and you cannot hear it.

How to work
- Do what was asked and no more. Work on the patch that is there: reuse its \
modules, keep the user's settings, do not rebuild or tidy what you were not \
asked to touch.
- Before each message you are given the patch as it is now. The user changes it \
too, so trust that over what you remember.
- The user is waiting. Describe what you need to know in one call, add all the \
modules in one call, then make all the cables and turn all the knobs, in one call \
each and both at once.
- add_modules answers with the new modules' knobs and jacks. Use the names it \
gives, without the quotation marks. Use describe for modules that are already in \
the patch, and only for those you are about to change.
- For anything that should sound: follow the signal to an Audio module of the \
patch (add one, Core/AudioInterface2, only if there is none), connect both left \
and right, and keep levels sane - outputs swing about 10 V, and a mixer or a \
VCA before the Audio module should leave it at about half. Then call listen \
once, and if what should move is steady at 0 V, find out why and fix it.
- Knobs sit at useful defaults. Set the ones the request is about.
- What is on no knob - a module's mode, its model, polyphony, presets - is in its \
menu: read_menu, then set_menu.
- Undo does not cover saving, opening and pausing. Do those only when asked.
- Pitch is 1 V per octave with 0 V = C4; gates and triggers are 0 or 10 V; \
modulation is +-5 V or 0-10 V. Most modulation inputs need their attenuverter \
knob turned up to do anything.
- Play mode is the user's: pressed in, the right stick (R3) starts and ends it. Then \
the patch is played with whatever of the controller it uses, through play_controls: \
both sticks, L2 and R2, A B X Y, L1 R1, the D-pad, L3 (left stick pressed in), tilt, \
twist and shake of the device, and on the screen a touch pad for two fingers, eight \
drum pads and four faders (they come up by themselves in play mode if used). Never \
START, SELECT and R3. What is not used goes on operating the rack, so do not take more \
than is asked for, and tell the user when the left stick (moves the view), L2 R2 \
(zoom), the D-pad or A B X Y (focus and editing) become the patch's while they play.
- Sticks, tilt and twist are -10 to 10 V with 0 at rest, the others 0 to 10 V. That is \
far too much for most inputs: bring it down with an attenuverter (Fundamental/8vert), \
the input's own, or a slot's lo and hi; for pitch 0.1 is an octave each way. Add it to \
what is there (a mixer that passes CV, or a stacked cable), and a quantizer keeps it in \
a scale. Unplug nothing the patch needs: it should still play by itself. Combinations \
(L1 held with a stick, L1 and A) and things that stay or switch are slots, \
play_assign. A jack is better than a knob; move a knob by a slot where there is none. \
One thing on each control, with a range that stays musical from end to end; say which \
does what, and that R3 starts it.
- The whole of one answer is one step of the user's Undo. If they want it \
undone, call undo.

- Making a module is for when the user asks for a module that is not there, or for \
a change to one made here (the plugin NovaMade). If an installed module does it, \
say so and use that, unless they want one made. Read module_guide first, once. Then \
make_module with probes that show it does what it should; if it is refused, put \
right what it says and call it again. Then add it to the patch, connect it if that \
was asked for, and measure. A module that is STOPPED has made a mistake: list_made \
says where, read_made shows it, edit_module puts it right.
- Undo does not cover making, changing and removing modules; revert_module does.

How to talk
- Answer in the user's language, in two short sentences, three at the most: the \
screen is four inches. No headings, no lists, no Markdown.
- Say what you made and one knob worth turning. Say so plainly when something \
could not be done, did not measure as it should, or changed more than was asked. \
Do not say you heard it, and do not explain that you cannot.
- Call a module by the name on its panel, which is the one read_patch and \
add_modules put first: Liquid Filter (AudibleInstruments/Ripples) is the Liquid \
Filter. Never by what stands behind the slash or by the Mutable Instruments module \
it copies (Ripples, Rings, Plaits, ...), even if the user does: that is nowhere on \
the screen. If the user uses such a name, say once which one it is. Where there are \
two of a kind, say which: by its place, or what it is connected to. Knobs and jacks \
by their names too. The numbers of modules are for your tools, not for the user.
- Nothing before or between tool calls: the user sees the patch change.
- A question gets an answer, not a change to the patch.
"""


# ---- The link to RackDroid ----

class Replaced(Exception):
    """RackDroid has another bridge, or wants one."""


def config():
    with open(CONFIG) as f:
        return json.load(f)


def session_id(text):
    return text if isinstance(text, str) and re.fullmatch(r"[0-9a-f-]{8,64}", text) else None


class Link:
    """One connection. `on_message` gets what is not an answer to ask().
    `instance` is the run of RackDroid that this was started by, if one was."""

    def __init__(self, role, on_message=None, instance=None):
        conf = config()
        token = conf["token"].encode()
        host = os.environ.get("NOVA_CHAT_HOST", "127.0.0.1")
        self.sock = socket.create_connection((host, conf["port"]), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.file = self.sock.makefile("r", encoding="utf-8")
        self.lock = threading.Lock()
        self.next_id = 0
        self.waiting = {}
        self.on_message = on_message
        hello = json.loads(self.file.readline() or "{}")
        theirs = hello.get("nonce", "")
        mine = secrets.token_hex(16)

        def proof(who):
            return hmac.new(token, f"{who}:{theirs}:{mine}".encode(), hashlib.sha256).hexdigest()
        me = {"role": role, "nonce": mine, "proof": proof("client")}
        if instance:
            me["instance"] = instance
        self.send(me)
        answer = json.loads(self.file.readline() or "{}")
        if not hmac.compare_digest(answer.get("proof", ""), proof("server")):
            self.sock.close()
            raise ConnectionError("that is not RackDroid, or the token is not its")
        if answer.get("replaced"):
            self.sock.close()
            raise Replaced()
        self.instance = answer.get("instance", "")
        self.settings = answer.get("settings") or {}
        self.session = session_id(answer.get("session"))
        self.sock.settimeout(None)
        self.closed = threading.Event()
        self.replaced = False
        threading.Thread(target=self._read, daemon=True).start()

    def send(self, o):
        data = (json.dumps(o, ensure_ascii=False) + "\n").encode()
        with self.lock:
            self.sock.sendall(data)

    def _read(self):
        try:
            for line in self.file:
                o = json.loads(line)
                box = self.waiting.pop(o.get("id"), None) if "ok" in o else None
                if box:
                    box.put(o)
                elif o.get("replaced"):
                    self.replaced = True
                elif self.on_message:
                    self.on_message(o)
        except (OSError, ValueError):
            pass
        self.closed.set()
        for box in list(self.waiting.values()):
            box.put({"ok": False, "error": "RackDroid closed the connection."})

    def ask(self, op, **args):
        with self.lock:
            self.next_id += 1
            n = self.next_id
        box = queue.Queue()
        self.waiting[n] = box
        try:
            self.send(dict(args, op=op, id=n))
            return box.get(timeout=125)
        except (OSError, queue.Empty):
            return {"ok": False, "error": "RackDroid does not answer."}
        finally:
            self.waiting.pop(n, None)


# ---- The patch as tools (MCP over stdin and stdout) ----

MODULE = {"type": "integer", "description": "A module's number in the patch."}
JACK = {"type": ["string", "integer"]}


def array(description, *items, minimum=None):
    """An array of small arrays: it is what is quickest for the model to write,
    and writing is most of what it spends its time on."""
    return {"type": "array", "description": description,
            "items": {"type": "array", "items": {"anyOf": list(items)}, "minItems": minimum or len(items),
                      "maxItems": len(items)}}


def model_list(a):
    return [dict({"op": "add", "model": m}, **({"right_of": a["right_of"]} if "right_of" in a and i == 0 else {}))
            for i, m in enumerate(a["models"])]


def cable_list(op):
    def make(a):
        out = []
        for c in a["cables"]:
            o = {"op": op, "from": c[0], "output": c[1], "to": c[2], "input": c[3]}
            if a.get("stack"):
                o["stack"] = True
            out.append(o)
        return out
    return make


def knob_list(a):
    out = []
    for module, knob, to in a["knobs"]:
        o = {"op": "set", "module": module, "param": knob}
        o["text" if isinstance(to, str) else "value"] = to
        out.append(o)
    return out


def one(op):
    return lambda a: [dict(a, op=op)]


def choice_list(a):
    out = []
    for c in a["choices"]:
        o = {"op": "choose", "module": a["module"], "item": str(c[0])}
        if len(c) > 1:
            o["text" if isinstance(c[1], str) else "value"] = c[1]
        out.append(o)
    return out


ACTIONS = ["bypass", "unbypass", "initialize", "randomize", "duplicate", "duplicate_with_cables", "unplug_all"]
WHERE = ["right_of", "left_of", "below", "above"]


CABLE = "Each cable is [module of the output, output, module of the input, input]; a jack by its name, or its number as listed (from 0)."

# Name, what it does, its parameters, those that must be there, and the
# operations (patch_ops.cpp) it stands for.
TOOLS = [
    ("read_patch", "The modules and cables of the patch as it is now.", {}, [], one("patch")),
    ("describe", "Modules of the patch: their knobs and where they stand, their jacks and what is plugged into them.",
     {"modules": {"type": "array", "items": MODULE}}, ["modules"],
     lambda a: [{"op": "module", "module": m} for m in a["modules"]]),
    ("find_modules", "Search the installed modules by name, kind or purpose (English or Japanese), with what each does.",
     {"query": {"type": "string"}}, ["query"], one("find")),
    ("describe_models", "The knobs and jacks of installed modules that are not in the patch.",
     {"models": {"type": "array", "items": {"type": "string", "description": "Plugin/Module, e.g. Fundamental/VCO."}}},
     ["models"], lambda a: [{"op": "model", "model": m} for m in a["models"]]),
    ("add_modules", "Add modules, left to right. Answers with their numbers, knobs and jacks. They go on the end of the row, or to the right of a module.",
     {"models": {"type": "array", "items": {"type": "string", "description": "Plugin/Module, e.g. Fundamental/VCO."}},
      "right_of": MODULE}, ["models"], model_list),
    ("remove_modules", "Take modules and their cables out of the patch.",
     {"modules": {"type": "array", "items": MODULE}}, ["modules"],
     lambda a: [{"op": "remove", "module": m} for m in a["modules"]]),
    ("connect", "Cables from outputs to inputs. What was in an input is unplugged, unless stack is true (then they are summed). " + CABLE,
     {"cables": array("[1, \"Sine\", 2, \"Audio\"], ...", MODULE, JACK, MODULE, JACK), "stack": {"type": "boolean"}},
     ["cables"], cable_list("connect")),
    ("disconnect", "Unplug cables. " + CABLE,
     {"cables": array("[1, \"Sine\", 2, \"Audio\"], ...", MODULE, JACK, MODULE, JACK)}, ["cables"], cable_list("disconnect")),
    ("set_knobs", "Turn knobs and switches. Each is [module, knob, where to]; the knob by its name or number, where to as a number in the range shown as [value ...], or as a text, what one would type at the knob: \"440\", \"C4\", \"50\" (in the unit it shows), or the name of a choice.",
     {"knobs": array("[2, \"Cutoff frequency\", \"800\"], ...", MODULE, JACK, {"type": ["number", "string"]}, minimum=3)},
     ["knobs"], knob_list),
    ("read_menu", "The own menu of a module - modes, models, options that are on no knob - and its presets. Each row has an id; what stands behind a name is its present state, a check mark or a value.",
     {"module": MODULE}, ["module"], one("menu")),
    ("set_menu", "Choose rows of a module's menu by the ids read_menu gave, in order. Each choice is [id], or [id, number] for a slider, or [id, text] for a text. Ids of presets start with p and load the preset.",
     {"module": MODULE, "choices": array("[\"2.1\"], ...", {"type": "string"}, {"type": ["number", "string"]}, minimum=1)},
     ["module", "choices"], choice_list),
    ("module_action", "Do something to modules as a whole. unplug_all takes out all their cables.",
     {"modules": {"type": "array", "items": MODULE}, "action": {"type": "string", "enum": ACTIONS}}, ["modules", "action"],
     lambda a: [{"op": "act", "module": m, "what": a["action"]} for m in a["modules"]]),
    ("move_modules", "Move modules, in order. Each move is [module, where, other module]; right_of and left_of make room, below and above are the neighbouring rows.",
     {"moves": array("[3, \"right_of\", 5], ...", MODULE, {"type": "string", "enum": WHERE}, MODULE)}, ["moves"],
     lambda a: [{"op": "move", "module": m[0], "where": m[1], "other": m[2]} for m in a["moves"]]),
    ("list_patches", "The saved patches, which one is open, and whether it has unsaved changes.", {}, [], one("patches")),
    ("save_patch", "Save the patch, under another name if one is given. Only when the user asks.",
     {"name": {"type": "string"}, "overwrite": {"type": "boolean", "description": "Only if the user said so."}}, [], one("save")),
    ("open_patch", "Open a saved patch, or a new one if no name is given, in place of the one that is open. Only when the user asks. If that has unsaved changes it is refused until the user has been asked: unsaved is keep (a copy is saved first) or discard.",
     {"name": {"type": "string"}, "unsaved": {"type": "string", "enum": ["keep", "discard"]}}, [], one("open")),
    ("play", "Let the engine run or pause it.", {"on": {"type": "boolean"}}, ["on"], one("play")),
    ("play_controls", "Nova Play: the controls of the handheld's play mode as voltages on named jacks, and eight slots. Added, or the one that is there. Connect its outputs as any other.",
     {"right_of": MODULE}, [], one("controls")),
    ("play_assign", "Set up a slot of Nova Play: a control of one's own, on the jack \"Slot n\", or moving a knob that has no jack. control and with are outputs' names of Nova Play, as play_controls lists them; with are up to two buttons to be held. mode: follow; stay (with a button held it is set and then stays, without one it is pushed up and down); gate; switch (on and off by turns); trigger. knob moves a knob by amount (-1 to 1) of its range away from where it stands, and back when let go. A control of null clears the slot.",
     {"module": MODULE, "slot": {"type": "integer"}, "control": {"type": ["string", "null"]},
      "with": {"type": "array", "items": {"type": "string"}, "maxItems": 2},
      "mode": {"type": "string", "enum": ["follow", "stay", "gate", "switch", "trigger"]},
      "lo": {"type": "number"}, "hi": {"type": "number"},
      "knob": {"type": "object", "properties": {"module": MODULE, "param": JACK, "amount": {"type": "number"}}, "required": ["module", "param", "amount"]}},
     ["module", "slot", "control"], one("assign")),
    ("listen", "Watch the outputs for a second and say how far each swings, including what reaches the speaker. Give a module to watch all of its outputs.",
     {"module": MODULE}, [], one("listen")),
    ("undo", "Undo the last answer's changes to the patch.", {}, [], one("undo")),
]

ID = {"type": "string", "description": "A capital letter, then letters and digits; also the name of the class."}
PROBES = {"type": "array", "maxItems": 6, "items": {"type": "object"}, "description": "As module_guide says."}

# The same, for modules made here; the operations are functions of Maker.
MAKING = [
    ("module_guide", "How a module is written. Read it before making one.", {}, []),
    ("list_made", "The modules made here: their versions, whether they run, and where they went wrong.", {}, []),
    ("read_made", "The source of a module made here.", {"id": ID, "version": {"type": "integer"}}, ["id"]),
    ("make_module", "Make a new module, or write one made here anew: it is compiled, tested and, if it passes, put in at once; nothing changes if not. The patch calls it NovaMade/id.",
     {"id": ID, "name": {"type": "string"}, "description": {"type": "string", "description": "One sentence."},
      "tags": {"type": "array", "items": {"type": "string"}, "description": "Rack's: Oscillator, Filter, Waveshaper, Effect, Delay, Sequencer, Utility, ..."},
      "code": {"type": "string"}, "probes": PROBES}, ["id", "name", "description", "tags", "code"]),
    ("edit_module", "Change the source of a module made here, by replacing pieces of it, and put it in as make_module does. Each edit is [old text, new text]; the old text must be there exactly once. Without edits it is only probed.",
     {"id": ID, "edits": {"type": "array", "items": {"type": "array", "items": {"type": "string"}, "minItems": 2, "maxItems": 2}},
      "probes": PROBES}, ["id", "edits"]),
    ("revert_module", "Go back to an earlier version of a module made here.", {"id": ID, "version": {"type": "integer"}}, ["id", "version"]),
    ("switch_module", "Start a module made here that is stopped, or stop it.", {"id": ID, "on": {"type": "boolean"}}, ["id", "on"]),
    ("remove_made", "Remove a module made here, for good. Only when the user asks, and not while it is in the patch.", {"id": ID}, ["id"]),
]


class Maker:
    """The tools of MAKING."""

    def __init__(self, ask, log, cancelled=lambda: False):
        self.ask = ask
        self.log = log
        self.cancelled = cancelled
        self.builder = None
        self.lib = None

    def _asked(self, op, **args):
        r = self.ask(op, **args)
        if not r.get("ok"):
            raise ValueError(r.get("error") or "RackDroid does not answer.")
        return r.get("text") or ""

    def _ready(self):
        """nova_build.py, out of RackDroid's APK like this file."""
        if self.builder:
            return
        kit = json.loads(self._asked("made_kit"))
        os.makedirs(MAKE, exist_ok=True)
        path = os.path.join(MAKE, "nova_build.py")
        beside = os.path.join(os.path.dirname(os.path.abspath(__file__)), "nova_build.py")
        if os.environ.get("NOVA_BUILD_HERE"):
            path = beside
        else:
            with zipfile.ZipFile(kit["apk"]) as z:
                data = z.read("assets/nova_build.py")
            with open(path, "wb") as f:
                f.write(data)
        spec = importlib.util.spec_from_file_location("nova_build", path)
        self.lib = importlib.util.module_from_spec(spec)
        sys.dont_write_bytecode = True
        spec.loader.exec_module(self.lib)
        self.apk = kit["apk"]
        self.builder = self.lib.Builder(kit["apk"], MAKE, self.log)

    def module_guide(self, a):
        self._ready()
        with zipfile.ZipFile(self.apk) as z:
            import io
            with zipfile.ZipFile(io.BytesIO(z.read("assets/module-kit.zip"))) as kit:
                return kit.read("guide.md").decode()

    def _lines(self, id, version, offset):
        """The lines of the source at a place in its library, if that is known."""
        debug = os.path.join(MAKE, "debug", f"{id}-{version}.so")
        if not offset or not os.path.exists(debug):
            return ""
        try:
            code = json.loads(self._asked("made_source", slug=id, version=version))["code"]
            self._ready()
            places = self.builder.where(debug, [offset], code)
            return " (" + "; ".join(places[:2]) + ")" if places else ""
        except (ValueError, OSError, KeyError):
            return ""

    def _faults(self):
        """Where modules went wrong since RackDroid was started."""
        out = {}
        for f in json.loads(self._asked("made_faults")):
            if f.get("slug"):
                out.setdefault(f["slug"], []).append(f"version {f['version']}: {f['what']}, {f['where']}"
                                                     + self._lines(f["slug"], f["version"], f.get("offset")))
        return out

    def list_made(self, a):
        made = json.loads(self._asked("made_list"))
        if not made:
            return "No module has been made here yet."
        faults = self._faults()
        out = []
        for m in made:
            s = f"{m['key']} \"{m['name']}\" - {m['description']}\n  version {m['version']} of {', '.join(map(str, m['versions']))}"
            s += f", {m['in_patch']} in the patch" if m["in_patch"] else ", not in the patch"
            if m["off"]:
                s += f"; STOPPED: {m['why']}" + self._lines(m["id"], m["version"], m.get("place"))
            elif m["stopped"]:
                s += f"; {m['stopped']}"
            elif m["stale"]:
                s += "; STOPPED: made for another version of RackDroid, edit_module without edits makes it again"
            for f in faults.get(m["id"], [])[-3:]:
                s += f"\n  went wrong: {f}"
            out.append(s)
        return "\n".join(out)

    def read_made(self, a):
        o = json.loads(self._asked("made_source", slug=a["id"], **({"version": a["version"]} if "version" in a else {})))
        return f"Version {o['version']}:\n{o['code']}"

    def _install(self, id, name, description, tags, code, probes, quiet=False):
        self._ready()
        if not re.fullmatch(r"[A-Z][A-Za-z0-9]{1,23}", id):
            raise ValueError("The id must be a capital letter and 1 to 23 letters and digits.")
        try:
            r = self.builder.build(id, code, probes or None)
        except self.lib.BuildError as e:
            raise ValueError(f"Not made; nothing has changed.\n{e}")
        except self.lib.Unavailable as e:
            raise ValueError(f"Modules cannot be made here: {e}")
        if self.cancelled():
            raise ValueError("Stopped by the user before it was put in.")
        meta = dict(r["describe"], name=name, description=description, tags=tags, abi=self.builder.abi)
        meta.pop("problems", None)
        text = self._asked("made_install", slug=id, meta=meta, code=code, library=base64.b64encode(r["so"]).decode())
        # With the lines in it, for when it goes wrong in RackDroid.
        m = re.search(r"is version (\d+)", text)
        if m:
            d = os.path.join(MAKE, "debug")
            os.makedirs(d, exist_ok=True)
            os.replace(r["debug"], os.path.join(d, f"{id}-{m.group(1)}.so"))
            old = [f for f in os.listdir(d) if f.startswith(id + "-")]
            old.sort(key=lambda f: int(re.sub(r"\D", "", f.rsplit("-", 1)[1]) or 0))
            for f in old[:-6]:
                os.remove(os.path.join(d, f))
        return f"{text}\n{r['report']}"

    def make_module(self, a):
        return self._install(a["id"], a["name"], a["description"], a["tags"], a["code"], a.get("probes"))

    def edit_module(self, a):
        o = json.loads(self._asked("made_source", slug=a["id"]))
        code = o["code"]
        for old, new in a["edits"]:
            n = code.count(old)
            if n != 1:
                raise ValueError(f"Nothing has changed: this is {'not there' if n == 0 else 'there %d times' % n}: {old[:200]}")
            code = code.replace(old, new)
        m = o["meta"]
        stale = m.get("abi") != json.loads(self._asked("made_kit"))["abi"]
        if not a["edits"] and not stale:
            self._ready()
            try:
                return "Not changed. " + self.builder.build(o["id"], code, a.get("probes"))["report"]
            except (self.lib.BuildError, self.lib.Unavailable) as e:
                raise ValueError(str(e))
        return self._install(o["id"], m["name"], m["description"], m.get("tags") or [], code, a.get("probes"))

    def revert_module(self, a):
        return self._asked("made_revert", slug=a["id"], version=a["version"])

    def switch_module(self, a):
        return self._asked("made_switch", slug=a["id"], on=a["on"])

    def remove_made(self, a):
        return self._asked("made_remove", slug=a["id"])

    def refresh(self):
        """Those made for another version of RackDroid, made again."""
        for m in json.loads(self._asked("made_list")):
            if not m["stale"] or m["off"]:
                continue
            try:
                o = json.loads(self._asked("made_source", slug=m["id"]))
                x = o["meta"]
                self._install(m["id"], x["name"], x["description"], x.get("tags") or [], o["code"], None)
                self.log(f"{m['id']} made again for this RackDroid")
            except (ValueError, OSError, KeyError) as e:
                self.log(f"{m['id']} cannot be made again: {e}")


def mcp():
    link = None
    tools = {t[0]: t for t in TOOLS}
    making = {t[0]: t for t in MAKING} if os.environ.get("NOVA_MAKING", "1") == "1" else {}
    given_up = set()
    current = None
    out_lock = threading.Lock()

    def answer(o):
        with out_lock:
            print(json.dumps(o, ensure_ascii=False), flush=True)

    def ask(op, **args):
        nonlocal link
        if link is None or link.closed.is_set():
            link = Link("tools")
        return link.ask(op, **args)

    maker = Maker(ask, log, lambda: current in given_up)

    def call(params):
        name = params.get("name")
        args = params.get("arguments") or {}
        if name in making:
            try:
                return getattr(maker, name)(args), True
            except (ValueError, KeyError, TypeError, IndexError) as e:
                return (str(e) if isinstance(e, ValueError) else f"Those arguments cannot be read ({e!r})."), False
            except OSError as e:
                return f"That cannot be done here ({e}).", False
        t = tools.get(name)
        if not t:
            return "No such tool.", False
        try:
            ops = t[4](args)
        except (KeyError, ValueError, TypeError, IndexError) as e:
            return f"Those arguments cannot be read ({e!r}).", False
        texts = []
        good = True
        for o in ops:
            try:
                r = ask(o.pop("op"), **o)
            except (OSError, ValueError, KeyError) as e:
                return f"RackDroid cannot be reached ({e}). Is its chat link on?", False
            good = good and bool(r.get("ok"))
            texts.append((r.get("text") or "Done.") if r.get("ok") else "ERROR: " + (r.get("error") or "?"))
        return "\n".join(x.rstrip("\n") for x in texts) or "Nothing to do.", good

    # One at a time, in the order asked: what comes later may build on what
    # came before. On a thread of its own, so that a call can be given up.
    calls = queue.Queue()

    def work():
        nonlocal current
        while True:
            m = calls.get()
            current = m["id"]
            if current in given_up:
                continue
            text, good = call(m["params"])
            if m["id"] not in given_up:
                answer({"jsonrpc": "2.0", "id": m["id"],
                        "result": {"content": [{"type": "text", "text": text}], "isError": not good}})
    threading.Thread(target=work, daemon=True).start()

    for line in sys.stdin:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if m.get("method") == "notifications/cancelled":
            given_up.add((m.get("params") or {}).get("requestId"))
        if "id" not in m or "method" not in m:
            continue
        reply = {"jsonrpc": "2.0", "id": m["id"]}
        if m["method"] == "initialize":
            reply["result"] = {"protocolVersion": m["params"]["protocolVersion"], "capabilities": {"tools": {}},
                               "serverInfo": {"name": "nova", "version": "1"}}
        elif m["method"] == "tools/list":
            reply["result"] = {"tools": [{"name": t[0], "description": t[1], "inputSchema": {
                "type": "object", "properties": t[2], "required": t[3], "additionalProperties": False}}
                for t in list(tools.values()) + list(making.values())]}
        elif m["method"] == "tools/call":
            calls.put(m)
            continue
        elif m["method"] == "ping":
            reply["result"] = {}
        else:
            reply["error"] = {"code": -32601, "message": "not here"}
        answer(reply)


# ---- Claude Code ----

def log(*what):
    print(time.strftime("%H:%M:%S"), *what, file=sys.stderr, flush=True)


class Claude:
    """One Claude Code process that takes a message after the other."""

    def __init__(self, link, catalog, session=None):
        os.makedirs(WORK, exist_ok=True)
        model = str(link.settings.get("model") or MODEL)
        if not re.fullmatch(r"[a-z0-9][a-z0-9.-]*(\[1m\])?", model):
            model = MODEL
        effort = link.settings.get("effort")
        effort = effort if effort in EFFORTS else EFFORT
        log(f"starting Claude Code: {model}, effort {effort}" + (", going on" if session else ""))
        config = os.path.join(WORK, "mcp.json")
        with open(config, "w") as f:
            json.dump({"mcpServers": {"nova": {
                "command": sys.executable, "args": ["-I", os.path.abspath(__file__), "mcp"],
                "env": {k: v for k, v in {"NOVA_CHAT_LINK": CONFIG,
                                          "NOVA_CHAT_HOST": os.environ.get("NOVA_CHAT_HOST"),
                                          "NOVA_MAKE": os.environ.get("NOVA_MAKE"),
                                          "NOVA_BUILD_HERE": os.environ.get("NOVA_BUILD_HERE"),
                                          "NOVA_MAKING": "0" if link.settings.get("making") is False else "1",
                                          "PATH": os.environ.get("PATH"), "HOME": os.environ.get("HOME"),
                                          **{k: v for k, v in os.environ.items() if k.startswith("ANDROID_")}}.items() if v}}}}, f)
        prompt = PROMPT
        if catalog:
            prompt += "\nThe installed modules, as Plugin/Module \"name\" [tags]:\n" + catalog + "\n"
        prompt_file = os.path.join(WORK, "prompt.txt")
        with open(prompt_file, "w") as f:
            f.write(prompt)
        cmd = [CLAUDE, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
               "--include-partial-messages", "--model", model, "--effort", effort,
               "--system-prompt-file", prompt_file, "--setting-sources", "",
               "--strict-mcp-config", "--mcp-config", config, "--tools", "", "--allowedTools", "mcp__nova"]
        # How long Claude Code keeps conversations, to go on with one from the list
        # in RackDroid; a month by itself.
        cmd += ["--settings", json.dumps({"cleanupPeriodDays": 365})]
        if session:
            cmd += ["--resume", session]
        # Whatever started this one must not be mistaken for its parent.
        env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDECODE") and k != "CLAUDE_CODE_ENTRYPOINT"}
        self.link = link
        self.session = session
        self.resuming = bool(session)
        self.unsaid = None
        self.forgotten = False
        self.closing = False
        self.started = time.time()
        self.busy = False
        self.stopped = False
        self.talked = False
        self.asked = 0.0
        self.calls = 0
        self.proc = subprocess.Popen(cmd, cwd=WORK, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE, text=True, encoding="utf-8")
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=self._complaints, daemon=True).start()

    def alive(self):
        return self.proc.poll() is None

    def _write(self, o):
        self.proc.stdin.write(json.dumps(o, ensure_ascii=False) + "\n")
        self.proc.stdin.flush()

    def say(self, text):
        self.busy = True
        self.stopped = False
        self.asked = time.time()
        self.calls = 0
        self.unsaid = text
        self._write({"type": "user", "message": {"role": "user", "content": text}})

    def stop(self):
        if self.busy and self.alive():
            self.stopped = True
            self._write({"type": "control_request", "request_id": secrets.token_hex(4),
                         "request": {"subtype": "interrupt"}})

    def close(self):
        self.closing = True
        if self.alive():
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def _event(self, **o):
        try:
            self.link.send(o)
        except OSError:
            pass

    def _complaints(self):
        for line in self.proc.stderr:
            log("claude:", line.rstrip())

    def _read(self):
        for line in self.proc.stdout:
            try:
                o = json.loads(line)
            except ValueError:
                continue
            kind = o.get("type")
            if kind == "system" and o.get("subtype") == "init":
                self.session = o.get("session_id")
                self.resuming = False
                self._event(event="session", id=self.session)
            elif kind == "stream_event":
                e = o.get("event", {})
                d = e.get("delta", {})
                block = e.get("content_block", {})
                if e.get("type") == "content_block_delta" and d.get("type") == "text_delta":
                    self._event(event="text", text=d.get("text", ""))
                elif e.get("type") == "content_block_start" and block.get("type") == "tool_use":
                    # Writing the source of a module takes a while.
                    self._event(event="doing", name=block.get("name", "").replace("mcp__nova__", ""))
            elif kind == "assistant":
                for block in o.get("message", {}).get("content", []):
                    if block.get("type") == "tool_use":
                        self.calls += 1
                        name = block.get("name", "").replace("mcp__nova__", "")
                        args = block.get("input", {})
                        if name in ("make_module", "edit_module"):
                            args = {k: v for k, v in args.items() if k in ("id", "name")}
                        self._event(event="tool", name=name, args=args, id=block.get("id"))
            elif kind == "user":
                for block in (o.get("message", {}).get("content") or []):
                    if isinstance(block, dict) and block.get("type") == "tool_result":
                        self._event(event="tool_done", id=block.get("tool_use_id"), ok=not block.get("is_error"))
            elif kind == "result":
                if self.resuming and o.get("is_error") and (o.get("num_turns") == 0 or "No conversation found" in json.dumps(o)):
                    # What was said is in RackDroid; what it came to is not here.
                    break
                self.busy = False
                self.talked = True
                seconds = time.time() - self.asked
                failed = bool(o.get("is_error"))
                how = "stopped" if self.stopped else "failed" if failed else "done"
                log(f"{how} after {seconds:.1f} s, {self.calls} tool calls", o.get("result") if how == "failed" else "")
                self._event(event="done", ok=not failed, stopped=self.stopped, text=o.get("result") or "",
                            seconds=round(seconds, 1), calls=self.calls)
        if self.resuming and not self.closing:
            self.forgotten = True
        elif self.busy:
            self.busy = False
            self._event(event="done", ok=False, text="Claude Code stopped.", seconds=0, calls=self.calls)


def run():
    inbox = queue.Queue()
    claude = None
    patience = float(os.environ.get("NOVA_CHAT_TIMEOUT", "0"))
    since = time.time()
    try:
        mine = config().get("instance")
    except (OSError, ValueError):
        mine = None
    while True:
        try:
            link = Link("bridge", inbox.put, mine)
        except Replaced:
            log("RackDroid was started again, and starts its own")
            if claude:
                claude.close()
            return
        except (OSError, ValueError, KeyError) as e:
            if patience and time.time() - since > patience:
                log("RackDroid has gone; so does this")
                if claude:
                    claude.close()
                return
            log("waiting for RackDroid:", e)
            time.sleep(2)
            continue
        log("connected to RackDroid")
        session = link.session
        catalog = link.ask("catalog").get("text", "")
        if claude:
            claude.close()
        # Ready before the first message: starting takes a while.
        try:
            claude = Claude(link, catalog, session)
        except OSError as e:
            link.send({"event": "bridge", "connected": True})
            link.send({"event": "error", "text": f"Claude Code cannot be started ({e})."})
            return
        link.send({"event": "bridge", "connected": True})
        threading.Thread(target=refresh, args=(link,), daemon=True).start()
        changed = False
        # The numbers of modules are of this run of RackDroid.
        renumbered = bool(session)
        while not link.closed.is_set():
            try:
                m = inbox.get(timeout=0.5)
            except queue.Empty:
                m = {}
            if claude.forgotten:
                # Claude Code has not got that conversation: without it, then.
                log("no conversation", session)
                unsaid = claude.unsaid if claude.busy else None
                claude.close()
                session = None
                link.send({"event": "forgot"})
                claude = Claude(link, link.ask("catalog").get("text", ""))
                if unsaid:
                    claude.say(unsaid)
            if "settings" in m:
                changed = changed or link.settings != m["settings"]
                link.settings = m["settings"]
            session = claude.session if claude.talked else session
            # Another model or effort is another process, with the same
            # conversation; it waits for the answer being given.
            if m.get("new") or "resume" in m or (changed and not claude.busy) or (m.get("chat") and not claude.alive()):
                claude.close()
                if m.get("new") or "resume" in m:
                    session = session_id(m.get("resume"))
                    renumbered = bool(session)
                claude = Claude(link, link.ask("catalog").get("text", ""), session)
                changed = False
            if m.get("stop"):
                claude.stop()
            if m.get("chat") and claude.busy:
                link.send({"event": "error", "text": "Still at work on the last one."})
            elif m.get("chat"):
                link.ask("turn")
                patch = link.ask("patch")
                now = patch.get("text") or patch.get("error") or ""
                try:
                    note = "(This conversation is from earlier: the numbers of modules are others now.)\n" if renumbered else ""
                    renumbered = False
                    claude.say(f"<patch>\n{now}</patch>\n{note}\n{m['chat']}")
                except OSError:
                    link.send({"event": "done", "ok": False, "text": "Claude Code could not be started.", "seconds": 0, "calls": 0})
        if link.replaced:
            log("another bridge has taken over")
            claude.close()
            return
        since = time.time()
        log("RackDroid went away")


def refresh(link):
    try:
        Maker(link.ask, log).refresh()
    except (OSError, ValueError, KeyError) as e:
        log("made modules:", e)


def say(text):
    over = threading.Event()

    def show(o):
        e = o.get("event")
        if e == "text":
            print(o["text"], end="", flush=True)
        elif e == "tool":
            print(f"\n  [{o['name']} {json.dumps(o['args'], ensure_ascii=False)}]", flush=True)
        elif e == "done":
            how = "stopped" if o.get("stopped") else "done" if o["ok"] else "failed: " + o["text"]
            print(f"\n-- {how}, {o['seconds']} s, {o['calls']} tool calls")
            over.set()
        elif e == "error":
            print(o["text"])
            over.set()
    link = Link("user", show)
    link.send({"new": True} if text == "/new" else {"stop": True} if text == "/stop" else {"chat": text})
    if not text.startswith("/"):
        over.wait(600)
    time.sleep(0.2)


def main():
    what = sys.argv[1] if len(sys.argv) > 1 else ""
    if what == "mcp":
        mcp()
    elif what == "run":
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        run()
    elif what == "say" and len(sys.argv) > 2:
        say(" ".join(sys.argv[2:]))
    elif what == "op" and len(sys.argv) > 2:
        link = Link("tools")
        for a in sys.argv[2:]:
            o = json.loads(a)
            r = link.ask(o.pop("op"), **o)
            print(("" if r.get("ok") else "ERROR ") + (r.get("text") or r.get("error") or "ok"))
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
