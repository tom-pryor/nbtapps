#!/usr/bin/env python3
# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
"""Builds NbtApp_ALL_Texts.zip (<lang>.txt with "id=text" lines, the format of BMW's PluginsCoded apps) and
NbtApp_ALL_Images.zip (<imageId>.png) into the given directory. Standard library only.

The zips are build products (`make pkg`), not tracked. Entries carry a fixed date, so the same inputs always give
the same bytes.

usage: tools/make_db.py [outdir]     (default: release/NbtApp)
"""
import os, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, "assets")

# One file per iDrive language; line 100000 is the language's own name. Keep texts ASCII.
LANGS = {"de": "Deutsch", "en-UK": "English (UK)", "en-US": "English (US)", "es-ES": "Espanol", "es-US": "Espanol (US)",
         "fr-FR": "Francais", "fr-CA": "Francais (CA)", "it": "Italiano", "pt": "Portugues", "ru": "Russian"}

# "\n" (backslash + n in the .txt) is a line break in a Richtext list.
ABOUT = ("NBTApp\\n"
         "Placeholder About screen.\\n"
         "Copyright (c) 2026 Tom Pryor - NBTApps\\n"
         "https://github.com/tom-pryor/nbtapps\\n"
         "PolyForm Strict License 1.0.0")

# Text ids: referenced by textId="..." in plugin/NbtApp.xml and by k...Text constants in src/ (tools/check.py).
# A row of a menu shows one line, so what a row reads is kept short.
TEXTS = {1: "NBTApp",   # entry row in Media/Radio, menu title (textIdModel 3)
         2: "About",    # About row and page title (textIdModel 36)
         3: ABOUT,      # About page text (src/nbt_app.cpp kAboutText)
         4: "Pair a phone",          # menu row and page title (textIdModel 38)
         5: "Forget all phones",     # menu row and page title (textIdModel 39)
         6: "Start the agent",       # menu row and page title (textIdModel 45)
         # Pair a phone, first row (textIdModel 40): src/nbt_app.cpp points it at one of these.
         10: "Asking the agent...",
         11: "Type this code into the phone app",
         12: "Phone paired",
         13: "3 wrong codes. Wait 30 seconds",
         14: "Code ran out. Open this again",
         15: "The agent is not running",         # on the Forget all phones and Start the agent pages too
         16: "Too soon. Wait 30 seconds",
         17: "Pairing did not work",
         20: "---",                              # a code's half while there is none (textIdModels 41, 42),
                                                 # and a row of Start the agent that does nothing (47)
         # Forget all phones: the first row (textIdModel 43), and what the second reads (textIdModel 44).
         30: "Every phone must pair again",
         31: "Forget them now",
         32: "Forgetting...",
         33: "No phone is paired now",
         34: "It did not work",
         # Start the agent (src/nbt_app.cpp kStart*Text): the first row (textIdModel 46), and the second (47)
         50: "Looking for the agent...",
         51: "The agent is running",
         52: "Start it now",
         53: "Starting the agent...",
         54: "Started. A phone can connect now",
         55: "The agent is not installed",
         56: "The agent did not start",
         57: "Try again"}

# The halves of a pairing code: text CODE_FIRST + n is the three digits of n, "000" to "999". The HMI shows texts
# of this file only, so every half a code can have is one (src/nbt_app.cpp kCodeFirstText).
CODE_FIRST = 1000
TEXTS.update({CODE_FIRST + n: "%03d" % n for n in range(1000)})

# Per-language overrides: {"de": {2: "Info"}, ...}
LANG_TEXTS = {
    "de": {2: "Info"},
    "es-ES": {2: "Acerca de"}, "es-US": {2: "Acerca de"},
    "fr-FR": {2: "A propos"}, "fr-CA": {2: "A propos"},
    "it": {2: "Informazioni"},
    "pt": {2: "Sobre"},
}

# Image ids: referenced by imageId="..." in plugin/NbtApp.xml. Files are in assets/.
IMAGES = {55001: "row_icon_63x54.png",            # imageIdModel 4: entry row icon
          55002: "content_preview_549x484.png"}   # imageIdModel 12: preview state 11

TEXTS_ZIP = "NbtApp_ALL_Texts.zip"
IMAGES_ZIP = "NbtApp_ALL_Images.zip"
FIXED_DATE = (2026, 1, 1, 0, 0, 0)


def add(z, name, data):
    info = zipfile.ZipInfo(name, date_time=FIXED_DATE)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = 0o644 << 16
    z.writestr(info, data)


def lang_file(lang):
    texts = dict(TEXTS)
    texts.update(LANG_TEXTS.get(lang, {}))
    body = "100000=%s\n" % LANGS[lang] + "".join("%d=%s\n" % (k, v) for k, v in sorted(texts.items()))
    return body.encode("utf-8")


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "release", "NbtApp")
    os.makedirs(out, exist_ok=True)
    with zipfile.ZipFile(os.path.join(out, TEXTS_ZIP), "w") as z:
        for lang in LANGS:
            add(z, lang + ".txt", lang_file(lang))
    with zipfile.ZipFile(os.path.join(out, IMAGES_ZIP), "w") as z:
        for image_id, name in sorted(IMAGES.items()):
            with open(os.path.join(ASSETS, name), "rb") as f:
                add(z, "%d.png" % image_id, f.read())
    print("wrote %s, %s in %s" % (TEXTS_ZIP, IMAGES_ZIP, os.path.relpath(out)))


if __name__ == "__main__":
    main()
