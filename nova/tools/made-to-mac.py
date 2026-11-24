#!/usr/bin/env python3
"""Modules made on the Nova from the chat, as a plugin for this Mac's VCV Rack.

    nova/tools/made-to-mac.py                 fetch, compile and install
    nova/tools/made-to-mac.py --user DIR      into another Rack user folder
    nova/tools/made-to-mac.py --sources DIR   from a folder, not from the Nova

The Nova keeps the source of each one, and what it is written against, in
Android/data/<app>/files/MadeModules. All of them make one plugin, NovaMade, as
they do on the Nova, so that a patch made there opens here. Restart Rack after.

NOVA_APP picks the app (org.rackdroid.test). Needs the Xcode command line tools,
and an Android build of this repository made once (rack-sdk-from-repo.sh).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.dont_write_bytecode = True
sys.path.insert(0, os.path.join(ROOT, "nova", "bridge"))
from nova_build import Builder  # noqa: E402

APP = os.environ.get("NOVA_APP", "org.rackdroid.test")
ADB = os.environ.get("ADB", os.path.expanduser("~/Library/Android/sdk/platform-tools/adb"))
WORK = os.path.join(ROOT, "nova", "downloads", "made")
SDK = os.path.join(ROOT, "nova", "downloads", "rack-sdk")

MAKEFILE = """\
RACK_DIR ?= {sdk}
FLAGS += -DNOVA_DESKTOP -Isrc
EXTRA_CXXFLAGS += -std=c++17
SOURCES += $(wildcard src/*.cpp)
DISTRIBUTABLES += res
include $(RACK_DIR)/plugin.mk
"""


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--user", help="Rack's user folder")
    p.add_argument("--sources", help="a folder with the sources")
    a = p.parse_args()

    sources = a.sources
    if not sources:
        sources = os.path.join(WORK, "MadeModules")
        shutil.rmtree(sources, ignore_errors=True)
        os.makedirs(WORK, exist_ok=True)
        remote = f"/sdcard/Android/data/{APP}/files/MadeModules"
        if subprocess.run([ADB, "pull", "-q", remote, WORK]).returncode:
            sys.exit("Nothing was fetched: is the Nova connected, and has a module been made on it?")
    metas = []
    for name in sorted(os.listdir(sources)):
        if name.endswith(".json"):
            with open(os.path.join(sources, name)) as f:
                metas.append((name[:-5], json.load(f)))
    if not metas:
        sys.exit("No module has been made on the Nova.")
    if not os.path.isdir(SDK):
        subprocess.run([os.path.join(ROOT, "nova", "tools", "rack-sdk-from-repo.sh")], check=True, cwd=ROOT)

    plugin = os.path.join(WORK, "plugin")
    src = os.path.join(plugin, "src")
    shutil.rmtree(plugin, ignore_errors=True)
    os.makedirs(src)
    os.makedirs(os.path.join(plugin, "res"))
    with open(os.path.join(plugin, "res", "README"), "w") as f:
        f.write("The panels are drawn.\n")
    for h in ("nova_kit.hpp", "nova_kit_impl.hpp"):
        shutil.copy(os.path.join(sources, h), src)
    wrap = Builder(None, None).source
    for id, meta in metas:
        with open(os.path.join(sources, id + ".cpp")) as f:
            code = f.read()
        with open(os.path.join(src, "made_" + id + ".cpp"), "w") as f:
            f.write('#include "nova_kit.hpp"\n' + wrap(id, code))
    with open(os.path.join(src, "plugin.cpp"), "w") as f:
        f.write("#include <rack.hpp>\n\n" + "".join(f"rack::plugin::Model* novaModel_{id}();\n" for id, _ in metas) +
                "\nvoid init(rack::plugin::Plugin* p) {\n" + "".join(f"\tp->addModel(novaModel_{id}());\n" for id, _ in metas) + "}\n")
    with open(os.path.join(plugin, "plugin.json"), "w") as f:
        json.dump({"slug": "NovaMade", "name": "Nova Made", "version": "2.0.0", "license": "GPL-3.0-or-later", "brand": "Nova Made",
                   "author": "Made on the Nova", "modules": [
                       {"slug": id, "name": m.get("name") or id, "description": m.get("description", ""), "tags": m.get("tags") or []}
                       for id, m in metas]}, f, indent=1)
    with open(os.path.join(plugin, "Makefile"), "w") as f:
        f.write(MAKEFILE.format(sdk=SDK))
    make = ["make", "-C", plugin, "-j6", "-s", "install"] + (["RACK_USER_DIR=" + os.path.abspath(a.user)] if a.user else [])
    r = subprocess.run(make, capture_output=True, text=True)
    if r.returncode:
        sys.exit("It does not compile here:\n" + (r.stdout + r.stderr)[-3000:])
    print("Installed NovaMade:", ", ".join(id for id, _ in metas))


if __name__ == "__main__":
    main()
