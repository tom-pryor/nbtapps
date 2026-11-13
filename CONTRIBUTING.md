# Contributing

Pull requests are welcome. Please read the agreement below first: opening a pull request means you accept it.

For a security problem, don't open a pull request or an issue: report it privately as [SECURITY.md](SECURITY.md)
says.

## Contributor agreement (v1.0)

By opening a pull request on this repository, or otherwise sending a contribution to this project, you agree to
the following for everything you contribute. It is the explicit agreement that applies to your contribution in
place of the licence GitHub's Terms of Service would otherwise give it (section D.6, "inbound = outbound").

1. You give Tom Pryor a perpetual, worldwide, non-exclusive, royalty-free, irrevocable licence to use, copy,
   change, distribute and sublicense your contribution, under any terms, including the license this project uses
   and any later one. You give the same for any patent of yours that the contribution needs.
2. You wrote the contribution, or you have the right to give it on these terms, including your employer's
   agreement if it has a claim to your work. Anything in it written by someone else is marked as such, with its
   licence.
3. You keep the copyright in your contribution. Nothing obliges us to use it, and you give it as is, without any
   warranty.

If you don't agree, please don't open a pull request.

## Before you open one

- **License headers.** Every new source file starts with the same two lines as the files beside it, in that
  file's comment syntax:

  ```
  SPDX-License-Identifier: LicenseRef-PolyForm-Strict-1.0.0
  Required Notice: Copyright (c) 2026 Tom Pryor - NBTApps (https://github.com/tom-pryor/nbtapps)
  ```

  `reuse lint` must pass. Test data, signed files and JSON don't take headers; [REUSE.toml](REUSE.toml) covers them.
- **Tests.**
  - `android/`: `./gradlew :app:testDebugUnitTest`.
  - `headunit/`: `make agent-test` (needs Docker; about a minute) and `make test-pkg`.
  - The QNX builds (`./build.sh`, `./build.sh agent`) need a licensed QNX SDK. If you don't have one, say so in the
    pull request and they'll be run for you.
- **Shared test files.** After a change to `headunit/spec/`, `headunit/tests/make_packages.py` or
  `headunit/plugin/NbtApp.xml`, run `android/tools/sync_agent_vectors.sh` and include what it changes. It needs
  Docker: the test packages are built in the agent's bench, so that their bytes are the same on every machine.
- **CI.** A pull request runs the checks in [.github/workflows/](.github/workflows/) on GitHub Actions: `reuse lint`
  and the header check on every one; on one that changes `android/` or `headunit/`, the Android unit tests with
  the real agent in its Docker bench (`NBT_REQUIRE_BENCH=1`, which fails the tests that would skip without the
  bench), the unsigned release build and lint. The head unit's own tests and the QNX builds are not run there.
- **No real car data.** No GPS positions, street names, VINs, Bluetooth addresses or bytes captured from a car in
  code, tests or comments. Make up values instead.
- **Names.** The head unit's app framework is the "HU Framework" in prose (the code's namespace stays `bmw`).
- **Car testing.** Say in the pull request whether you tried the change on a car, and which head unit. Pull
  requests are squash-merged, so that goes in the description, not in commit messages.
