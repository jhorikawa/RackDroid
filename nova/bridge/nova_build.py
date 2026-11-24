#!/usr/bin/env python3
"""Makes a module for RackDroid out of C++ on the device: compiles it, tries it
out in a process of its own, and hands back what goes into the app.

    nova_build.py APK WORK build SLUG FILE.cpp [PROBES.json]   try it from a shell

The code may be wrong; that is expected. It is compiled with checks that stay in
(every index of a vector or an array), tested with more of them (address and
undefined behaviour sanitizers) in every way a module can be used, and only what
gets through that is given to RackDroid, which guards it once more
(native/port/module_guard.cpp).

It is also looked at before it runs anywhere: no #include, no raw pointers, and
nothing imported but arithmetic, Rack and the C++ library - no files, no network,
no programs. That is there for mistakes; it is not a wall against malice.

The compiler is Termux's (pkg install clang llvm); the headers and the libraries
come out of RackDroid's own APK.

Python 3.9 or later, and nothing else.
"""
import base64
import contextlib
import fcntl
import glob
import hashlib
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import zipfile

TERMUX = os.environ.get("NOVA_TERMUX", "/data/data/com.termux/files/usr")

BASE = ["-std=gnu++17", "-fPIC", "-DANDROID", "-DGLFW_INCLUDE_NONE", "-D_RACK_VERSION=2.6.4",
        "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE", "-fno-omit-frame-pointer",
        "-mno-omit-leaf-frame-pointer", "-mno-outline-atomics", "-DNOVA_SPLIT", "-ffunction-sections", "-w"]
# What is found while it is tested ...
# (-O0: checks that were merged into one would have no line.)
CHECK = ["-O0", "-g", "-DNOVA_TEST", "-fsanitize=address,undefined,float-cast-overflow",
         "-fno-sanitize=vptr,function", "-fno-sanitize-recover=all"]
# ... and what stays in, for what was not.
RELEASE = ["-O3", "-gline-tables-only", "-fsanitize=bounds,return,unreachable", "-fsanitize-trap=all", "-fno-sanitize-merge"]
# For the module's own code, not for the headers.
WARN = ["-Wno-everything", "-Wunsafe-buffer-usage", "-Wreturn-type", "-Wuninitialized", "-Wsometimes-uninitialized",
        "-Winfinite-recursion", "-Warray-bounds", "-Wdivision-by-zero", "-Wdelete-non-abstract-non-virtual-dtor",
        "-Wdangling", "-Wreturn-stack-address", "-Wformat", "-Werror", "-ferror-limit=12"]

# A budget at 48 kHz is 20833 ns a frame, for the whole patch.
TOO_SLOW = 8000
SLOW = 2500

# What the module's own code may ask for; names are as the linker has them. The
# rest of Rack and of the system is out of reach: files, the network, programs,
# threads, the patch.
ALLOWED = re.compile(
    r"^(_ZNK?4rack(6engine(?!6Engine)|6string|3dsp|6random|4math|8Quantity)|_ZN4rack3app12ModuleWidget8setModel|"
    r"_ZN4rack6plugin6Plugin8addModel|_ZNK?4nova|_ZT[IVS]N4(nova|rack6engine|rack8Quantity)|_ZT[IVS]N4rack(6engine|8Quantity)|"
    r"_ZNK?St|_ZSt|_ZNKSt|_ZT[ISV]N?K?(St|10__cxxabiv1)|_ZN10__cxxabiv1|__cxa_|__gxx_|__dynamic_cast$|__dso_handle$|"
    r"_GLOBAL_OFFSET_TABLE_$|__assert2$|_Unwind_Resume$|_Zn[wa]m|_Zd[la]P|nvg(?!Create|Delete|Internal)|"
    r"json_(object|array|integer|real|string|number|boolean|true|false|null|delete|is)|nova_guard_|__stack_chk|"
    r"mem(cpy|move|set|cmp|chr)$|str(len|cmp|ncmp|chr|rchr)$|__(memcpy|memmove|memset|strlen|strcpy|strcat|vsnprintf|snprintf)_chk$|"
    r"v?snprintf$|abort$|"
    r"(a?(sin|cos|tan)|sincos|atan2|(sin|cos|tan)h|a(sin|cos|tan)h|exp|exp2|exp10|expm1|log|log2|log10|log1p|pow|sqrt|cbrt|hypot|"
    r"floor|ceil|round|lround|llround|lrint|llrint|rint|trunc|nearbyint|fmod|modf|remainder|fmin|fmax|fdim|fma|copysign|ldexp|frexp|"
    r"scalbn|erf|erfc|tgamma|lgamma|nextafter|nan|fabs|ilogb|logb)[fl]?$|__\w*[ts]f\w*[0-9]?$)")
REFUSED = re.compile(r"thread|mutex|condition_variable|__fs|filesystem|filebuf|fstream|future|random_device|_ZN4nova9makeWidgetE?$")

# Words the module's own code may not have, and what to do instead.
WORDS = [
    (r"^\s*#", "lines that begin with # (no #include, no #define): everything there is, is included; use constexpr"),
    (r"\b(new|delete)\b(?!\s*;)", "new and delete: use std::vector, std::array or std::make_unique"),
    (r"\b(malloc|calloc|realloc|free|alloca)\s*\(", "malloc and the like: use std::vector"),
    (r"\b(reinterpret_cast|const_cast)\b", "reinterpret_cast and const_cast"),
    (r"\b(asm|__asm__?|__attribute__|__declspec|extern|goto|setjmp|longjmp|volatile)\b", "asm, attributes, extern, goto, volatile"),
    (r"\b(APP|contextGet|contextSet)\b", "APP: the rate is in sampleRate and args.sampleRate, and nothing else of the app is needed"),
    (r"\b(thread|mutex|async|future|condition_variable|jthread)\b", "threads and locks: process() must never wait"),
    (r"\b(system|popen|fork|exec[lv]\w*|fopen|ifstream|ofstream|fstream|filesystem|socket|dlopen|dlsym|getenv)\s*[\(<: ]",
     "files, programs, the network"),
    (r"\bstd::(cout|cerr|printf)|\b(printf|fprintf|puts)\s*\(", "printing: there is nowhere to print to"),
    (r"\btypedef\b.*\(\s*\*|\buintptr_t\b|\bintptr_t\b", "pointers as numbers"),
]


class BuildError(Exception):
    """What is wrong, for whoever wrote the code."""


class Unavailable(Exception):
    """It cannot be built here at all."""


def stripped(code):
    """The code without comments and the insides of strings, the lines kept."""
    def blank(m):
        s = m.group(0)
        if s[0] == "/":
            return re.sub(r"[^\n]", " ", s)
        return s[0] + " " * (len(s) - 2) + s[-1]
    return re.sub(r"//[^\n]*|/\*.*?\*/|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])+'", blank, code, flags=re.S)


def look(slug, code):
    """What can be said without compiling."""
    bare = stripped(code)
    found = []
    for pattern, what in WORDS:
        for m in re.finditer(pattern, bare, re.M):
            word = m.group(0).strip()
            # "= delete" and the like are no harm.
            if word == "delete" and re.search(r"=\s*$", bare[:m.start()]):
                continue
            found.append(f"line {bare.count(chr(10), 0, m.start()) + 1}: \"{word}\" - not allowed: {what}")
            break
    if not re.search(r"\b(struct|class)\s+" + re.escape(slug) + r"\s*(final\s*)?:\s*(public\s+)?(nova::)?Module\b", bare):
        found.append(f"there is no \"struct {slug} : nova::Module\": the class must have the module's id as its name")
    if "process" not in bare:
        found.append("there is no process()")
    if len(code) > 60000:
        found.append("that is too long")
    if found:
        raise BuildError("\n".join(found))


def digest(entries):
    h = hashlib.sha256()
    for name, crc in sorted(entries):
        h.update(f"{name} {crc:08x}\n".encode())
    return h.hexdigest()[:16]


class Builder:
    def __init__(self, apk, work, log=lambda *a: None):
        self.apk = apk
        self.work = work
        self.log = log
        self.kit = None

    @contextlib.contextmanager
    def _alone(self):
        """The bridge and what Claude Code has started may both be at it."""
        os.makedirs(self.work, exist_ok=True)
        with open(os.path.join(self.work, "lock"), "w") as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            yield

    # ---- The kit ----

    def tool(self, name):
        path = os.path.join(TERMUX, "bin", name)
        if not os.path.exists(path):
            raise Unavailable(f"{name} is not installed. In Termux: pkg install clang llvm")
        return path

    def prepare(self):
        """Unpacks and compiles what does not change from module to module. A
        while the first time with each version of RackDroid, then nothing."""
        if self.kit:
            return
        clang = self.tool("clang++")
        self.tool("llvm-nm")
        try:
            with zipfile.ZipFile(self.apk) as z:
                inner = z.read("assets/module-kit.zip")
                self.abi = z.read("assets/module-kit.abi").decode().strip()
                libs = {n: z.getinfo("lib/arm64-v8a/" + n).CRC for n in ("librack_engine.so", "libc++_shared.so")}
        except (OSError, KeyError, zipfile.BadZipFile) as e:
            raise Unavailable(f"this RackDroid does not carry what modules are made with ({e})")
        import io
        with zipfile.ZipFile(io.BytesIO(inner)) as z:
            entries = [(i.filename, i.CRC) for i in z.infolist() if not i.is_dir()]
        compiler = subprocess.run([clang, "--version"], capture_output=True, text=True).stdout.split("\n")[0]
        with open(os.path.abspath(__file__), "rb") as f:
            me = hashlib.sha256(f.read())
        me.update((compiler + digest(entries + list(libs.items()))).encode())
        kit = os.path.join(self.work, "kit", me.hexdigest()[:16])
        if not os.path.exists(os.path.join(kit, "ready")):
            self.log("preparing the module kit")
            started = time.time()
            for old in glob.glob(os.path.join(self.work, "kit", "*")):
                shutil.rmtree(old, ignore_errors=True)
            os.makedirs(kit)
            with zipfile.ZipFile(io.BytesIO(inner)) as z:
                z.extractall(kit)
            os.makedirs(os.path.join(kit, "lib"))
            with zipfile.ZipFile(self.apk) as z:
                for n in libs:
                    with open(os.path.join(kit, "lib", n), "wb") as f:
                        f.write(z.read("lib/arm64-v8a/" + n))
            with open(os.path.join(kit, "all.hpp"), "w") as f:
                f.write("#include <nova_kit.hpp>\n")
            self._at(kit)
            jobs = []
            for kind, flags in (("check", CHECK), ("release", RELEASE)):
                jobs.append([clang, *BASE, *flags, *self.includes, "-x", "c++-header", os.path.join(kit, "all.hpp"),
                             "-o", os.path.join(kit, kind + ".pch")])
                jobs.append([clang, *BASE, *flags, *(["-O2"] if kind == "release" else []), *self.includes, "-x", "c++",
                             os.path.join(kit, "nova_kit_impl.hpp"), "-c", "-o", os.path.join(kit, kind + "-kit.o")])
            jobs.append([clang, *BASE, *CHECK, *self.includes, os.path.join(kit, "test.cpp"), "-o", os.path.join(kit, "test-check"), *self.links, "-rdynamic"])
            jobs.append([clang, *BASE, "-O2", *self.includes, os.path.join(kit, "test.cpp"), "-o", os.path.join(kit, "test-speed"), *self.links, "-rdynamic"])
            for code, text in self._all(jobs, 240):
                if code:
                    raise Unavailable("the kit does not compile here: " + text[-1500:])
            with open(os.path.join(kit, "ready"), "w") as f:
                f.write("%.1f s" % (time.time() - started))
            self.log("the module kit is ready after %.1f s" % (time.time() - started))
        self._at(kit)
        self.kit = kit

    def _at(self, kit):
        # All of it as system headers: what is looked for in a module's own code
        # is not looked for in them.
        self.includes = ["-isystem", kit]
        for d in sorted(os.listdir(os.path.join(kit, "include"))):
            self.includes += ["-isystem", os.path.join(kit, "include", d)]
        self.links = [os.path.join(kit, "lib", "librack_engine.so"), os.path.join(kit, "lib", "libc++_shared.so"), "-nostdlib++", "-lm", "-ldl"]

    def _env(self, kit=None):
        kit = kit or self.kit
        # Not Termux's libraries: the system's own, which RackDroid's need, would
        # find theirs among them.
        rt = glob.glob(os.path.join(TERMUX, "lib/clang/*/lib/linux"))
        env = {k: v for k, v in os.environ.items() if k in ("PATH", "HOME", "TMPDIR") or k.startswith("ANDROID_")}
        env["LD_LIBRARY_PATH"] = ":".join([os.path.join(kit, "lib")] + rt)
        env["ASAN_OPTIONS"] = ("symbolize=0:handle_sigtrap=1:handle_sigill=1:handle_sigfpe=1:handle_abort=1:detect_leaks=0:"
                               "max_allocation_size_mb=256:hard_rss_limit_mb=1500:allocator_may_return_null=0:"
                               "detect_odr_violation=0:abort_on_error=0:print_legend=0:malloc_context_size=2")
        env["UBSAN_OPTIONS"] = "symbolize=0:print_stacktrace=1"
        return env

    @staticmethod
    def _run(cmd, timeout, env=None, cwd=None):
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                             text=True, errors="replace", env=env, cwd=cwd, start_new_session=True)
        try:
            return p.communicate(timeout=timeout)[0], p.returncode, False
        except subprocess.TimeoutExpired:
            # Where it is, if it can still say so.
            p.send_signal(signal.SIGTRAP)
            try:
                return p.communicate(timeout=4)[0], p.returncode, True
            except subprocess.TimeoutExpired:
                p.kill()
                return p.communicate()[0], p.returncode, True

    def _all(self, jobs, timeout):
        out = [None] * len(jobs)

        def one(i, job):
            text, code, late = self._run(job, timeout)
            out[i] = (1 if late else code, text)
        threads = [threading.Thread(target=one, args=x) for x in enumerate(jobs)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        return out

    # ---- One module ----

    def source(self, slug, code):
        return (f"namespace made_{slug} {{\n"
                "using namespace rack;\n"
                "using nova::Delay; using nova::Filter; using nova::Smooth;\n"
                "using nova::knob; using nova::big; using nova::small; using nova::trim; using nova::in; using nova::out;\n"
                "using nova::light; using nova::gap;\n"
                "using Module = nova::Module;\n"
                f"#line 1 \"{slug}.cpp\"\n{code}\n"
                "#line 1 \"the kit\"\n"
                "}\n"
                f"NOVA_MODULE(made_{slug}, {slug})\n")

    def build(self, slug, code, probes=None):
        """Returns {"so": bytes, "debug": path, "describe": ..., "report": text}.
        Raises BuildError with what is wrong."""
        look(slug, code)
        with self._alone():
            self.prepare()
            return self._build(slug, code, probes)

    def _build(self, slug, code, probes):
        started = time.time()
        kit = self.kit
        clang = self.tool("clang++")
        d = os.path.join(self.work, "build", slug)
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
        with open(os.path.join(d, "module.cpp"), "w") as f:
            f.write(self.source(slug, code))
        bad = []

        def compile_one(kind, flags):
            base = [x for x in BASE if x != "-w"]
            result = self._run([clang, *base, *flags, *self.includes, *WARN, "-include-pch", os.path.join(kit, kind + ".pch"),
                                "-Xclang", "-fno-pch-timestamp", "-c", "module.cpp", "-o", kind + ".o"], 120, cwd=d)
            if result[1] == 0 and not result[2]:
                sanitize = [f for f in flags if f.startswith("-fsanitize=address")]
                result = self._run([clang, *sanitize, "-shared", kind + ".o", os.path.join(kit, kind + "-kit.o"), "-o", kind + ".so", *self.links,
                                    "-Wl,-soname,libnova_made_" + slug + ".so", "-Wl,-z,max-page-size=16384", "-Wl,-Bsymbolic",
                                    "-Wl,--exclude-libs,ALL", "-Wl,--gc-sections"], 60, cwd=d)
            if result[1] or result[2]:
                bad.append(result)
        threads = [threading.Thread(target=compile_one, args=x) for x in (("check", CHECK), ("release", RELEASE))]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        if bad:
            raise BuildError("It does not compile:\n" + self._errors(bad[0][0] if not bad[0][2] else "the compiler took too long"))
        self._imports(os.path.join(d, "release.o"))
        compiled = time.time()

        if probes:
            with open(os.path.join(d, "probes.json"), "w") as f:
                json.dump(probes, f)
        out = {}

        def check():
            t = time.time()
            out["check"] = self._run([os.path.join(kit, "test-check"), "check", os.path.join(d, "check.so")], 45, self._env(), d)
            self.log("check %.1f s" % (time.time() - t))

        def speed():
            t = time.time()
            out["speed"] = self._run([os.path.join(kit, "test-speed"), "speed", os.path.join(d, "release.so")] +
                                     ([os.path.join(d, "probes.json")] if probes else []), 45, self._env(), d)
            self.log("speed %.1f s" % (time.time() - t))
        threads = [threading.Thread(target=check), threading.Thread(target=speed)]
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        lines = self._judge(out["check"], os.path.join(d, "check.so"), code)
        describe = json.loads(next(x["describe"] for x in lines if "describe" in x))
        if describe["problems"]:
            raise BuildError("It works, but:\n" + "\n".join(describe["problems"]))
        fast = self._judge(out["speed"], os.path.join(d, "release.so"), code)
        report = self._report(describe, lines, fast)

        so = os.path.join(d, "release.so")
        small = os.path.join(d, "stripped.so")
        text, code_, late = self._run([self.tool("llvm-strip"), "--strip-unneeded", so, "-o", small], 30)
        if code_:
            raise BuildError("It cannot be stripped: " + text[-300:])
        with open(small, "rb") as f:
            data = f.read()
        self.log("compiled in %.1f s, tested in %.1f s" % (compiled - started, time.time() - compiled))
        return {"so": data, "debug": so, "describe": describe, "report": report}

    def _errors(self, text):
        """The compiler's errors without what is not the writer's to change."""
        keep = []
        for block in re.split(r"\n(?=\S+?:\d+:\d+: )", text):
            first = block.split("\n")[0]
            if "error:" in first and len(keep) < 6:
                first = re.sub(r" \[-Werror,(-W[\w-]+)\]", r" [\1]", first)
                if "unsafe-buffer-usage" in first:
                    first = first.split(": error:")[0] + (": error: a raw pointer or a C array is indexed or counted up, which cannot be "
                                                         "checked: use std::array, std::vector or std::string")
                source = [x for x in block.split("\n")[1:3] if "|" in x]
                keep.append("\n".join([first] + source[:1]))
            elif "note:" in first and keep and len(keep) < 6 and re.match(r"\w+\.cpp:", first):
                keep.append("  " + first)
        if not keep:
            keep = [x for x in text.split("\n") if "error" in x or "undefined" in x][:6] or [text[-600:]]
        return "\n".join(keep)[:3500]

    def _imports(self, o):
        """Of the object file: the library has the compiler's own besides."""
        text, code, late = self._run([self.tool("llvm-nm"), "--undefined-only", "--no-sort", o], 30)
        bad = []
        for line in text.split("\n"):
            name = line.split()[-1].split("@")[0] if line.split() else ""
            if not name:
                continue
            if REFUSED.search(name) or not ALLOWED.match(name):
                bad.append(name)
        if bad:
            names, _, _ = self._run([self.tool("llvm-cxxfilt"), *bad[:8]], 30)
            raise BuildError("It uses what a module may not use (files, the network, threads and the like are out of reach): " +
                             ", ".join(names.split("\n")[:8]).strip(", "))

    def _symbolize(self, so, offsets):
        """[(function, line in the module's code), ...]"""
        if not offsets:
            return []
        text, code, late = self._run([self.tool("llvm-symbolizer"), "--obj=" + so, "--functions=short", "--inlining=true",
                                      "--output-style=JSON"] + ["0x%x" % o for o in offsets], 40)
        found = []
        for line in text.split("\n"):
            try:
                o = json.loads(line)
            except ValueError:
                continue
            for s in [x for one in (o if isinstance(o, list) else [o]) for x in one.get("Symbol") or []]:
                name = os.path.basename(s.get("FileName") or "")
                if name.endswith(".cpp") and name not in ("module.cpp", "test.cpp"):
                    found.append((s.get("FunctionName") or "", s.get("Line") or 0))
        return found

    def where(self, so, offsets, code):
        out = []
        lines = code.split("\n")
        for function, line in self._symbolize(so, offsets):
            text = lines[line - 1].strip() if 0 < line <= len(lines) else ""
            place = f"line {line}, in {function.split('(')[0]}: {text}"
            if text and place not in out:
                out.append(place)
        return out[:4]

    def _judge(self, result, so, code):
        """The lines of JSON it answered, if all went well."""
        text, status, late = result
        lines = []
        rest = []
        for line in text.split("\n"):
            try:
                o = json.loads(line)
                lines.append(o if isinstance(o, dict) else {})
            except ValueError:
                rest.append(line)
        rest = "\n".join(rest)
        at = next((x["at"] for x in reversed(lines) if "at" in x), "starting")
        if any(x.get("sandbox") is False for x in lines):
            raise Unavailable("the test cannot be kept from the rest of the device")
        failed = next((x for x in lines if "fail" in x), None)
        if failed:
            raise BuildError(f"The test failed at {failed['at']}: {failed['fail']}")
        if status == 0 and not late and any("ok" in x for x in lines):
            return lines
        name = os.path.basename(so)
        offsets = [int(x, 16) for x in re.findall(re.escape(name) + r"\+0x([0-9a-f]+)\)", rest)]
        what = "it did not come back: a loop without an end, or far too slow" if late else ""
        if not what:
            m = re.search(r"runtime error: (.*)", rest)
            if m:
                what = m.group(1)
                here = re.search(r"(\w+\.cpp):(\d+):\d+: runtime error", rest)
                if here and not offsets:
                    line = int(here.group(2))
                    src = code.split("\n")
                    raise BuildError(f"The test failed at {at}: {what}\n  line {line}: {src[line - 1].strip() if 0 < line <= len(src) else ''}")
        if not what:
            m = re.search(r"ERROR: AddressSanitizer:? (.*)", rest)
            says = re.search(r"assertion \"(.*)\" failed", rest)
            if "hard rss limit" in rest:
                what = "it takes more and more memory"
            elif m and "requested allocation size" in m.group(1):
                what = "it asks for far too much memory, perhaps in a loop without an end"
            elif m and "Alarm clock" in rest:
                what = "it did not come back: a loop without an end, or far too slow"
            elif says:
                what = f"Rack stopped it: {says.group(1)}"
            elif m:
                what = re.sub(r" on (unknown )?address .*| \(pc .*", "", m.group(1))
                what = {"TRAP": "a check failed: an index out of range of a vector or an array (or something as impossible)",
                        "ILL": "a check failed",
                        "ABRT": "it was aborted: an exception nobody caught, or a check that failed",
                        "SEGV": "a memory access that is not allowed"}.get(what.split(" ")[0], what)
        if not what:
            m = re.search(r"^fault (\d+)", rest, re.M)
            if m:
                what = {"5": "a check failed: an index out of range of a vector or an array (or something as impossible), or a call that did not come back",
                        "4": "a check failed", "6": "it was aborted: an exception nobody caught, or a check that failed"}.get(
                            m.group(1), "a memory access that is not allowed")
        if not what:
            what = f"it died ({'signal %d' % -status if status < 0 else 'status %d' % status})"
            tail = [x for x in rest.split("\n") if x.strip()][-2:]
            if tail:
                what += ": " + " ".join(tail)[:300]
        places = self.where(so, offsets[:12], code)
        raise BuildError(f"The test failed at {at}: {what}" + "".join("\n  " + p for p in places))

    @staticmethod
    def _report(describe, lines, fast):
        speed = next(x["speed"] for x in fast if "speed" in x)
        ok = next(x["ok"] for x in lines if "ok" in x)
        ns, ns16 = speed["ns"], speed["ns16"]
        if ns > TOO_SLOW:
            raise BuildError(f"It works, but it is too slow: {ns:.0f} ns a frame, {ns / 208.33:.0f}% of a processor core at 48 kHz. "
                             f"At most {TOO_SLOW} ns.")
        out = [f"{describe['width']} HP wide. One of it takes {ns / 208.33:.1f}% of a processor core at 48 kHz ({ns:.0f} ns a frame"
               + (f", {ns16:.0f} with 16 channels in)" if ns16 > ns * 1.3 else ")") + "."]
        if ns > SLOW:
            out.append("That is a lot for a handheld: make it lighter if you can.")
        soak = next((x["soak"]["seconds"] for x in fast if "soak" in x), 0)
        out.append(f"{soak} s of it ran without a fault." + ("" if soak >= 119 else " (Too slow to try two minutes.)"))
        if speed["allocations"]:
            out.append(f"process() asked for memory {speed['allocations']} times in 2000 frames, which can make the sound drop out: "
                       "make vectors and strings in the constructor.")
        if ok["peak"] > 20:
            out.append(f"An output reached {ok['peak']:.0f} V under rough treatment.")
        for i, p in enumerate(x["probe"] for x in fast if "probe" in x):
            text = []
            if p.get("error"):
                text.append("ERROR " + p["error"])
            for name, o in (p.get("outputs") or {}).items():
                s = f"{name} {o['min']:.3g} to {o['max']:.3g} V" if o["max"] - o["min"] > 1e-4 else f"{name} steady at {o['max']:.4g} V"
                if "hz" in o:
                    s += f", {o['hz']:.5g} Hz"
                if o["max"] - o["min"] > 1e-4:
                    s += f", mean {o['mean']:.3g}, {o.get('end', 0):.3g} at the end"
                if o.get("channels", 1) > 1:
                    s += f", {o['channels']} channels"
                text.append(s)
            out.append(f"Probe {i + 1}: " + "; ".join(text))
        return "\n".join(out)


def main():
    if len(sys.argv) < 6 or sys.argv[3] != "build":
        print(__doc__)
        return 2
    apk, work, _, slug, path = sys.argv[1:6]
    with open(path) as f:
        code = f.read()
    probes = None
    if len(sys.argv) > 6:
        with open(sys.argv[6]) as f:
            probes = json.load(f)
    b = Builder(apk, work, lambda *a: print(*a, file=sys.stderr))
    t = time.time()
    try:
        r = b.build(slug, code, probes)
        print(r["report"])
        print(json.dumps(r["describe"]))
        print(len(r["so"]), "bytes", len(base64.b64encode(r["so"])), "as text")
    except BuildError as e:
        print("NOT BUILT\n" + str(e))
    except Unavailable as e:
        print("UNAVAILABLE", e)
    print("%.1f s" % (time.time() - t))
    return 0


if __name__ == "__main__":
    sys.exit(main())
