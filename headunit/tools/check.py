#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""`make check`: host-side checks that need no car and no QNX toolchain. Exits non-zero on any failure.

Source checks (always):
  - plugin/NbtApp.xml and NbtApp.manifest are well-formed; ids are unique; every reference inside the XML resolves
    (actions, models, states), every textId is in tools/make_db.py TEXTS, every imageId in IMAGES with its file
  - constants in src/ named k...Model / k...Action / k...State / k...Text / k...Image hold ids that exist
  - the manifest names the files the build produces
Release checks (when release/NbtApp exists; otherwise reported as skipped):
  - the folder holds exactly the five files, and the DBs hold the ids above
  - libNbtApp.so passes the head unit's ABI gate: ELF32 little-endian ARM shared object, EABI5, soft-float calls,
    soname, the expected NEEDED list, pluginAppInfo exported, every HU Framework import listed in stubs/*.syms
  - the library says it is the version src/version.hpp names, which is the version of its package
Package checks (when release/pkg holds the plugin's package of that version, `make plugin-pkg`; otherwise skipped):
  - release/pkg/org.nbtapp.plugin-<version>.nbtpkg is a package, not signed, and byte for byte what
    tools/plugin_pkg.py makes of release/NbtApp now. Its manifest places that folder as ${plugins}/NbtApp/ and
    nothing else, asks for an HMI restart, and names the agent it requires
Needs readelf (binutils) for the library checks.
"""
import glob, os, re, shutil, subprocess, sys, zipfile
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import make_db
import nbtpkg
import plugin_pkg

NAME = "NbtApp"
XML = os.path.join(ROOT, "plugin", NAME + ".xml")
MANIFEST = os.path.join(ROOT, "plugin", NAME + ".manifest")
RELEASE = os.path.join(ROOT, "release", NAME)
LIB = "lib%s.so" % NAME
RELEASE_FILES = {LIB, NAME + ".manifest", NAME + ".xml", make_db.TEXTS_ZIP, make_db.IMAGES_ZIP}
# libsocket: the plugin's link to the agent is a local socket (src/agent_link.hpp).
NEEDED = {"libAppFramework.A.so", "libCoreFramework.A.so", "libstdc++.so.6", "libm.so.2", "libc.so.3", "libsocket.so.3"}

failures = []


def fail(msg):
    failures.append(msg)
    print("FAIL  " + msg)


def ok(msg):
    print("ok    " + msg)


def ids_of(parent, what):
    """id -> element for the children of `parent`; reports duplicates."""
    found = {}
    for el in parent if parent is not None else []:
        i = el.get("id")
        if i is None:
            continue
        if i in found:
            fail("%s id %s is used twice" % (what, i))
        found[i] = el
    return found


def check_xml():
    """Returns (model ids, action ids, state ids, text ids used, image ids used) over all pluginApps."""
    all_models, all_actions, all_states = set(), set(), set()
    try:
        root = ET.parse(XML).getroot()
    except ET.ParseError as e:
        fail("plugin/%s.xml is not well-formed: %s" % (NAME, e))
        return all_models, all_actions, all_states
    before = len(failures)
    focus_ids = [a.get("entryButtonFocusID") for a in root.iter("pluginApp") if a.get("entryButtonFocusID")]
    if len(focus_ids) != len(set(focus_ids)):
        fail("entryButtonFocusID is used twice")
    for app in root.iter("pluginApp"):
        models = ids_of(app.find("models"), "model")
        actions = ids_of(app.find("actions"), "action")
        states = ids_of(app.find("hmiStates"), "hmiState")
        components = {}
        for state in states.values():
            for comp in state.find("components") if state.find("components") is not None else []:
                if comp.get("id") in components:
                    fail("component id %s is used twice" % comp.get("id"))
                components[comp.get("id")] = comp

        def ref(el, attr, table, what):
            v = el.get(attr)
            if v is not None and v not in table:
                fail('<%s id="%s"> %s="%s": no such %s' % (el.tag, el.get("id"), attr, v, what))

        entry = app.find("entryButton")
        if entry is None:
            fail("pluginApp has no entryButton")
        else:
            ref(entry, "action", actions, "action")
            for attr in ("model", "imageModel", "previewModel"):
                ref(entry, attr, models, "model")
        for a in actions.values():
            ref(a, "target", states, "hmiState")
        for m in models.values():
            ref(m, "previewState", states, "hmiState")
            if m.tag == "textIdModel" and int(m.get("textId", "-1")) not in make_db.TEXTS:
                fail('textIdModel %s: textId %s is not in tools/make_db.py TEXTS' % (m.get("id"), m.get("textId")))
            if m.tag == "imageIdModel" and int(m.get("imageId", "-1")) not in make_db.IMAGES:
                fail('imageIdModel %s: imageId %s is not in tools/make_db.py IMAGES' % (m.get("id"), m.get("imageId")))
        for s in states.values():
            ref(s, "textModel", models, "model")
            ref(s, "parentId", states, "hmiState")
        for c in components.values():
            ref(c, "action", actions, "action")
            for attr in ("model", "textModel"):
                ref(c, attr, models, "model")
        for ev in app.find("events") if app.find("events") is not None else []:
            ref(ev, "target", states, "hmiState")
            ref(ev, "targetModel", models, "model")
        all_models |= set(models)
        all_actions |= set(actions)
        all_states |= set(states)
    if len(failures) == before:
        ok("plugin/%s.xml: %d models, %d actions, %d states, all references resolve"
           % (NAME, len(all_models), len(all_actions), len(all_states)))
    return all_models, all_actions, all_states


def check_db_sources():
    before = len(failures)
    for image_id, name in make_db.IMAGES.items():
        if not os.path.isfile(os.path.join(make_db.ASSETS, name)):
            fail("image %d: assets/%s is missing" % (image_id, name))
    for text_id, text in make_db.TEXTS.items():
        if not all(ord(ch) < 128 for ch in text):
            fail("text %d is not ASCII" % text_id)
    for lang, overrides in make_db.LANG_TEXTS.items():
        if lang not in make_db.LANGS:
            fail("LANG_TEXTS has unknown language %s" % lang)
        for text_id in overrides:
            if text_id not in make_db.TEXTS:
                fail("LANG_TEXTS[%s] overrides text %d, which TEXTS does not have" % (lang, text_id))
    if len(failures) == before:
        ok("tools/make_db.py: %d texts, %d images, %d languages" % (len(make_db.TEXTS), len(make_db.IMAGES), len(make_db.LANGS)))


def check_source_constants(models, actions, states):
    """const unsigned kAboutModel = 37;  ->  37 must be a model id. The name's suffix says which table."""
    tables = {"Model": (models, "model id in the XML"), "Action": (actions, "action id in the XML"),
              "State": (states, "hmiState id in the XML"),
              "Text": ({str(k) for k in make_db.TEXTS}, "text id in tools/make_db.py"),
              "Image": ({str(k) for k in make_db.IMAGES}, "image id in tools/make_db.py")}
    pattern = re.compile(r"\bconst\s+unsigned\s+(k\w*?(Model|Action|State|Text|Image))\s*=\s*(\d+)\s*;")
    before, count = len(failures), 0
    for path in sorted(glob.glob(os.path.join(ROOT, "src", "*.[ch]pp"))):
        with open(path) as f:
            for lineno, line in enumerate(f, 1):
                for name, kind, value in pattern.findall(line):
                    count += 1
                    table, what = tables[kind]
                    if value not in table:
                        fail("%s:%d: %s = %s is not a %s" % (os.path.relpath(path, ROOT), lineno, name, value, what))
    if len(failures) == before:
        ok("src/: %d id constants match the XML and the DBs" % count)


def check_manifest():
    try:
        root = ET.parse(MANIFEST).getroot()
    except ET.ParseError as e:
        fail("plugin/%s.manifest is not well-formed: %s" % (NAME, e))
        return
    before = len(failures)
    expect = {"name": NAME, "file": "{pluginDir}/" + NAME,
              "config/parameters/DescFile": "{pluginDir}/%s.xml" % NAME,
              "config/parameters/TextDBFile": "{pluginDir}/" + make_db.TEXTS_ZIP,
              "config/parameters/ImageDBFile": "{pluginDir}/" + make_db.IMAGES_ZIP}
    for path, want in expect.items():
        got = (root.findtext(path) or "").strip()
        if got != want:
            fail("manifest <%s> is '%s', expected '%s'" % (path, got, want))
    if len(failures) == before:
        ok("plugin/%s.manifest names the files the build produces" % NAME)


def check_release_files():
    before = len(failures)
    present = set(os.listdir(RELEASE))
    for name in sorted(RELEASE_FILES - present):
        fail("release/%s/%s is missing" % (NAME, name))
    for name in sorted(present - RELEASE_FILES):
        fail("release/%s/%s should not be there (iDrive loads what it finds)" % (NAME, name))
    for name in (NAME + ".xml", NAME + ".manifest"):
        src, dst = os.path.join(ROOT, "plugin", name), os.path.join(RELEASE, name)
        if os.path.isfile(dst) and open(src, "rb").read() != open(dst, "rb").read():
            fail("release/%s/%s differs from plugin/%s: run make pkg" % (NAME, name, name))
    texts = os.path.join(RELEASE, make_db.TEXTS_ZIP)
    if os.path.isfile(texts):
        with zipfile.ZipFile(texts) as z:
            for lang in make_db.LANGS:
                if lang + ".txt" not in z.namelist():
                    fail("%s has no %s.txt" % (make_db.TEXTS_ZIP, lang))
                elif z.read(lang + ".txt") != make_db.lang_file(lang):
                    fail("%s %s.txt differs from tools/make_db.py: run make pkg" % (make_db.TEXTS_ZIP, lang))
    images = os.path.join(RELEASE, make_db.IMAGES_ZIP)
    if os.path.isfile(images):
        with zipfile.ZipFile(images) as z:
            for image_id, name in make_db.IMAGES.items():
                entry = "%d.png" % image_id
                if entry not in z.namelist():
                    fail("%s has no %s" % (make_db.IMAGES_ZIP, entry))
                elif z.read(entry) != open(os.path.join(make_db.ASSETS, name), "rb").read():
                    fail("%s %s differs from assets/%s: run make pkg" % (make_db.IMAGES_ZIP, entry, name))
    if len(failures) == before:
        ok("release/%s: the five files, DBs match the sources" % NAME)


def readelf(*args):
    return subprocess.run(["readelf", "-W"] + list(args), check=True, stdout=subprocess.PIPE,
                          universal_newlines=True).stdout


def check_library():
    path = os.path.join(RELEASE, LIB)
    if not os.path.isfile(path):
        return
    if not shutil.which("readelf"):
        fail("readelf not found: cannot check %s" % LIB)
        return
    before = len(failures)
    header = readelf("-h", path)

    def field(name):
        m = re.search(r"^\s*%s:\s*(.*)$" % re.escape(name), header, re.M)
        return m.group(1).strip() if m else ""

    if field("Class") != "ELF32" or "little endian" not in field("Data") or field("Machine") != "ARM":
        fail("%s is not ELF32 little-endian ARM (%s, %s, %s)" % (LIB, field("Class"), field("Data"), field("Machine")))
    if not field("Type").startswith("DYN"):
        fail("%s is not a shared object (%s)" % (LIB, field("Type")))
    flags = int(field("Flags").split(",")[0], 16) if field("Flags") else 0
    if flags >> 24 != 5:
        fail("%s is not EABI version 5 (flags 0x%x)" % (LIB, flags))
    if flags & 0x400 or "Tag_ABI_VFP_args" in readelf("-A", path):
        fail("%s uses the hard-float call ABI; the head unit is soft-float" % LIB)

    dynamic = readelf("-d", path)
    soname = re.findall(r"\(SONAME\).*\[(.*)\]", dynamic)
    if soname != [LIB]:
        fail("%s soname is %s, expected %s" % (LIB, soname, LIB))
    needed = set(re.findall(r"\(NEEDED\).*\[(.*)\]", dynamic))
    if needed != NEEDED:
        fail("%s NEEDED is %s, expected %s" % (LIB, sorted(needed), sorted(NEEDED)))

    exported, imported = {}, set()
    for line in readelf("--dyn-syms", path).splitlines():
        cols = line.split()
        if len(cols) == 8 and cols[0].rstrip(":").isdigit():
            if cols[6] == "UND":
                imported.add(cols[7])
            elif cols[4] in ("GLOBAL", "WEAK"):
                exported[cols[7]] = cols[3]
    if exported.get("pluginAppInfo") != "OBJECT":
        fail("%s does not export the pluginAppInfo object" % LIB)
    framework = set()
    for syms in glob.glob(os.path.join(ROOT, "stubs", "*.syms")):
        with open(syms) as f:
            framework |= {line.split()[1] for line in f if line[:2] in ("F ", "O ")}
    for sym in sorted(s for s in imported if "3bmw" in s and s not in framework):
        fail("%s imports %s, which the HU Framework does not export (stubs/*.syms)" % (LIB, sym))
    if len(failures) == before:
        ok("%s: ELF32 ARM EABI5 soft-float shared object, soname and NEEDED as expected, pluginAppInfo exported, "
           "%d HU Framework imports all known" % (LIB, sum(1 for s in imported if "3bmw" in s)))


def check_version():
    """The plugin's version, as src/version.hpp names it and as the library that was built says it."""
    try:
        version = plugin_pkg.source_version()
    except plugin_pkg.Refused as e:
        fail(str(e))
        return None
    ok("src/version.hpp: the plugin is version %s" % version)
    path = os.path.join(RELEASE, LIB)
    if os.path.isfile(path):
        with open(path, "rb") as f:
            said = plugin_pkg.build_line(f.read())
        if said is None:
            fail("%s does not say which version it is and which commit it was built from" % LIB)
        elif said[0] != version:
            fail("%s says it is version %s, and src/version.hpp says %s: build again" % (LIB, said[0], version))
        else:
            ok("%s: version %s, build %s" % (LIB, said[0], said[1]))
    return version


def check_package(version):
    """The plugin's own package, as `make plugin-pkg` left it in release/pkg."""
    name = plugin_pkg.archive_name(version)
    shown = os.path.join("release", "pkg", name)
    path = os.path.join(plugin_pkg.PKG_DIR, name)
    if not os.path.isfile(path):
        print("skip  %s does not exist: make plugin-pkg makes the plugin's package" % shown)
        return
    before = len(failures)
    try:
        _, fresh, manifest = plugin_pkg.build(RELEASE, version)
    except plugin_pkg.Refused as e:
        fail("the plugin's package cannot be made of release/%s: %s" % (NAME, e))
        return
    template = "plugin/package/manifest.json.in"
    install = manifest.get("install", {})
    files = [(f.get("src"), f.get("dest"), f.get("mode")) for f in install.get("files", [])]
    if set(install) != {"files", "reboot"} or files != [("payload/%s/" % NAME, "${plugins}/%s/" % NAME, "0644")]:
        fail("%s: the plugin's package places its folder as ${plugins}/%s/ with mode 0644, and does nothing else" % (template, NAME))
    if install.get("reboot") != "hmi-restart":
        fail("%s: install.reboot is not hmi-restart: the HMI loads a plugin only when it starts" % template)
    if "agent" not in manifest.get("requires", {}):
        fail("%s: requires.agent is missing: it says which agents can install this plugin" % template)
    with open(path, "rb") as f:
        data = f.read()
    try:
        info = nbtpkg.verify_package(data, None, allow_unsigned=True)
    except nbtpkg.PkgError as e:
        fail("%s is not a package: %s" % (shown, e))
        return
    if "signature" in info:
        fail("%s is signed: `make plugin-release` puts the signed archive in release/pkg/signed" % shown)
    elif data != fresh:
        fail("%s is not the package of release/%s: make plugin-pkg" % (shown, NAME))
    if len(failures) == before:
        ok("%s: version %s, the folder release/%s, %d bytes, not signed" % (shown, version, NAME, len(data)))


def main():
    models, actions, states = check_xml()
    check_db_sources()
    check_source_constants(models, actions, states)
    check_manifest()
    version = check_version()
    if os.path.isdir(RELEASE):
        check_release_files()
        check_library()
        if version is not None:
            check_package(version)
    else:
        print("skip  release/%s does not exist: build first to check the release folder and the library" % NAME)
    if failures:
        print("\n%d check(s) failed" % len(failures))
        return 1
    print("\nall checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
