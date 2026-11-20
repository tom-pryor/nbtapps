# SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
# Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
# Project-specific ProGuard/R8 rules.

# JSch (data/ssh/). It makes the classes of its algorithms by name, from the names in its config table
# (Class.forName(...).getDeclaredConstructor().newInstance()). Without these the shrinker removes them, and JSch then
# leaves out the algorithms it cannot load without a word, or fails on the first it needs (com.jcraft.jsch.jce.Random).
-keep class com.jcraft.jsch.jce.** { <init>(); }
-keep class com.jcraft.jsch.bc.** { <init>(); }
-keep class com.jcraft.jsch.DH* { <init>(); }
-keep class com.jcraft.jsch.UserAuth* { <init>(); }
-keep class com.jcraft.jsch.CipherNone { <init>(); }
# Some of those (DHEC256, DH25519, ...) are not public, and are made by name from JSch's own package: the shrinker must
# not move JSch's classes out of it, nor rename them.
-keepnames class com.jcraft.jsch.**
