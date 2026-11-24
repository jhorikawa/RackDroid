#!/usr/bin/env python3
"""Between RackDroid's chat and Claude Code, on the same device.

    nova_bridge.py run            answer what is typed in RackDroid's chat
    nova_bridge.py say TEXT...    type something there from a shell
    nova_bridge.py op JSON...     one operation on the patch, for trying things
    nova_bridge.py mcp            Claude Code starts this one: the patch as tools

Claude Code is run as it was published and as it was signed in by whoever owns
the device; nothing here sees or handles that. It is given the tools below and
no others: no files, no shell, no web.

RackDroid starts this itself, in Termux, when its chat is opened (TermuxBridge.kt)
and hands it the port and the token. To try things from elsewhere, copy the app's
chat-link.json to ~/.nova-chat-link.json, or name it in NOVA_CHAT_LINK.

Python 3.9 or later, and nothing else.
"""
import hashlib
import hmac
import json
import os
import queue
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time

CONFIG = os.environ.get("NOVA_CHAT_LINK") or os.path.expanduser("~/.nova-chat-link.json")
CLAUDE = os.environ.get("NOVA_CLAUDE", "claude")
MODEL = os.environ.get("NOVA_CHAT_MODEL", "opus")
EFFORT = os.environ.get("NOVA_CHAT_EFFORT", "low")
WORK = os.path.expanduser("~/.nova-chat")

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
- Pitch is 1 V per octave with 0 V = C4; gates and triggers are 0 or 10 V; \
modulation is +-5 V or 0-10 V. Most modulation inputs need their attenuverter \
knob turned up to do anything.
- The whole of one answer is one step of the user's Undo. If they want it \
undone, call undo.

How to talk
- Answer in the user's language, in two short sentences, three at the most: the \
screen is four inches. No headings, no lists, no Markdown.
- Say what you made and one knob worth turning. Say so plainly when something \
could not be done, did not measure as it should, or changed more than was asked. \
Do not say you heard it, and do not explain that you cannot.
- Nothing before or between tool calls: the user sees the patch change.
- A question gets an answer, not a change to the patch.
"""


# ---- The link to RackDroid ----

class Link:
    """One connection. `on_message` gets what is not an answer to ask()."""

    def __init__(self, role, on_message=None):
        with open(CONFIG) as f:
            conf = json.load(f)
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
        self.send({"role": role, "nonce": mine, "proof": proof("client")})
        answer = json.loads(self.file.readline() or "{}")
        if not hmac.compare_digest(answer.get("proof", ""), proof("server")):
            self.sock.close()
            raise ConnectionError("that is not RackDroid, or the token is not its")
        self.instance = answer.get("instance", "")
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
            return box.get(timeout=20)
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
    ("listen", "Watch the outputs for a second and say how far each swings, including what reaches the speaker. Give a module to watch all of its outputs.",
     {"module": MODULE}, [], one("listen")),
    ("undo", "Undo the last answer's changes to the patch.", {}, [], one("undo")),
]


def mcp():
    link = None
    tools = {t[0]: t for t in TOOLS}

    def answer(o):
        print(json.dumps(o, ensure_ascii=False), flush=True)

    def call(params):
        nonlocal link
        t = tools.get(params.get("name"))
        if not t:
            return "No such tool.", False
        try:
            ops = t[4](params.get("arguments") or {})
        except (KeyError, ValueError, TypeError, IndexError) as e:
            return f"Those arguments cannot be read ({e!r}).", False
        texts = []
        good = True
        for o in ops:
            try:
                if link is None or link.closed.is_set():
                    link = Link("tools")
                r = link.ask(o.pop("op"), **o)
            except (OSError, ValueError, KeyError) as e:
                return f"RackDroid cannot be reached ({e}). Is its chat link on?", False
            good = good and bool(r.get("ok"))
            texts.append((r.get("text") or "Done.") if r.get("ok") else "ERROR: " + (r.get("error") or "?"))
        return "\n".join(x.rstrip("\n") for x in texts) or "Nothing to do.", good

    # One at a time, in the order asked: what comes later may build on what
    # came before.
    for line in sys.stdin:
        try:
            m = json.loads(line)
        except ValueError:
            continue
        if "id" not in m or "method" not in m:
            continue
        reply = {"jsonrpc": "2.0", "id": m["id"]}
        if m["method"] == "initialize":
            reply["result"] = {"protocolVersion": m["params"]["protocolVersion"], "capabilities": {"tools": {}},
                               "serverInfo": {"name": "nova", "version": "1"}}
        elif m["method"] == "tools/list":
            reply["result"] = {"tools": [{"name": n, "description": d, "inputSchema": {
                "type": "object", "properties": p, "required": r, "additionalProperties": False}}
                for n, d, p, r, _ in TOOLS]}
        elif m["method"] == "tools/call":
            text, good = call(m["params"])
            reply["result"] = {"content": [{"type": "text", "text": text}], "isError": not good}
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
        config = os.path.join(WORK, "mcp.json")
        with open(config, "w") as f:
            json.dump({"mcpServers": {"nova": {
                "command": sys.executable, "args": ["-I", os.path.abspath(__file__), "mcp"],
                "env": {k: v for k, v in {"NOVA_CHAT_LINK": CONFIG,
                                          "NOVA_CHAT_HOST": os.environ.get("NOVA_CHAT_HOST")}.items() if v}}}}, f)
        prompt = PROMPT
        if catalog:
            prompt += "\nThe installed modules, as Plugin/Module \"name\" [tags]:\n" + catalog + "\n"
        prompt_file = os.path.join(WORK, "prompt.txt")
        with open(prompt_file, "w") as f:
            f.write(prompt)
        cmd = [CLAUDE, "-p", "--input-format", "stream-json", "--output-format", "stream-json", "--verbose",
               "--include-partial-messages", "--model", MODEL, "--effort", EFFORT,
               "--system-prompt-file", prompt_file, "--setting-sources", "",
               "--strict-mcp-config", "--mcp-config", config, "--tools", "", "--allowedTools", "mcp__nova"]
        if session:
            cmd += ["--resume", session]
        # Whatever started this one must not be mistaken for its parent.
        env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDECODE") and k != "CLAUDE_CODE_ENTRYPOINT"}
        self.link = link
        self.session = session
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
        self._write({"type": "user", "message": {"role": "user", "content": text}})

    def stop(self):
        if self.busy and self.alive():
            self.stopped = True
            self._write({"type": "control_request", "request_id": secrets.token_hex(4),
                         "request": {"subtype": "interrupt"}})

    def close(self):
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
            elif kind == "stream_event":
                e = o.get("event", {})
                d = e.get("delta", {})
                if e.get("type") == "content_block_delta" and d.get("type") == "text_delta":
                    self._event(event="text", text=d.get("text", ""))
            elif kind == "assistant":
                for block in o.get("message", {}).get("content", []):
                    if block.get("type") == "tool_use":
                        self.calls += 1
                        name = block.get("name", "").replace("mcp__nova__", "")
                        self._event(event="tool", name=name, args=block.get("input", {}))
            elif kind == "result":
                self.busy = False
                self.talked = True
                if self.session:
                    remember(self.link.instance, self.session)
                seconds = time.time() - self.asked
                failed = bool(o.get("is_error"))
                how = "stopped" if self.stopped else "failed" if failed else "done"
                log(f"{how} after {seconds:.1f} s, {self.calls} tool calls", o.get("result") if how == "failed" else "")
                self._event(event="done", ok=not failed, stopped=self.stopped, text=o.get("result") or "",
                            seconds=round(seconds, 1), calls=self.calls)
        if self.busy:
            self.busy = False
            self._event(event="done", ok=False, text="Claude Code stopped.", seconds=0, calls=self.calls)


def remembered(instance):
    """The conversation of this run of RackDroid, if there was one."""
    try:
        with open(os.path.join(WORK, "session.json")) as f:
            o = json.load(f)
        return o["session"] if o["instance"] == instance else None
    except (OSError, ValueError, KeyError):
        return None


def remember(instance, session):
    with open(os.path.join(WORK, "session.json"), "w") as f:
        json.dump({"instance": instance, "session": session}, f)


def run():
    inbox = queue.Queue()
    claude = None
    patience = float(os.environ.get("NOVA_CHAT_TIMEOUT", "0"))
    since = time.time()
    while True:
        try:
            link = Link("bridge", inbox.put)
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
        session = remembered(link.instance)
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
        while not link.closed.is_set():
            try:
                m = inbox.get(timeout=1)
            except queue.Empty:
                continue
            session = claude.session if claude.talked else session
            if m.get("new") or not claude.alive():
                claude.close()
                session = None if m.get("new") else session
                remember(link.instance, session)
                claude = Claude(link, link.ask("catalog").get("text", ""), session)
                link.send({"event": "bridge", "connected": True})
            if m.get("stop"):
                claude.stop()
            if m.get("chat") and claude.busy:
                link.send({"event": "error", "text": "Still at work on the last one."})
            elif m.get("chat"):
                link.ask("turn")
                patch = link.ask("patch")
                now = patch.get("text") or patch.get("error") or ""
                try:
                    claude.say(f"<patch>\n{now}</patch>\n\n{m['chat']}")
                except OSError:
                    link.send({"event": "done", "ok": False, "text": "Claude Code could not be started.", "seconds": 0, "calls": 0})
        if link.replaced:
            log("another bridge has taken over")
            claude.close()
            return
        since = time.time()
        log("RackDroid went away")


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
        over.wait(180)
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
