# Security

## Reporting a problem

Please report a security problem privately, not in a public issue: on this repo's **Security** tab, choose
**Report a vulnerability**. Say what is affected (the agent, the app or the plugin, and its version), what an
attacker can do, and how to reproduce it.

There is no fixed response time. A fix goes out as a new release of the part affected, through the catalogue.

## What is in scope

- The install agent (`nbtagent`, `nbtctl`, the launcher and starter scripts): the package checks (signature,
  manifest, hashes, archive paths), the install engine, pairing, and the protocol a phone speaks to it over
  Bluetooth serial or the local socket.
- The Android app (`com.nbtapps.store`): the catalogue checks, pairing and the keys it keeps, and what it sends to
  a head unit.
- The iDrive plugin (NBTApp).

## What is not

- The keys in `headunit/tests/keys/`. Their private halves are public on purpose; only tests and the debug
  build's sample catalogue trust them.
- Getting a root shell on a head unit. The agent is installed from one, and this repo does not cover how to get it.
- The TCP link of the agent's host build (`--debug-tcp`) and of the app's debug build, which are for the test
  bench only and are not in the builds that go to a car or a phone.

## Versions

Only the newest release of the agent and of the app is fixed. Update through the app to get a fix.
