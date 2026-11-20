# Security

## Reporting a problem

Please report a security problem privately, not in a public issue: on this repo's **Security** tab, choose
**Report a vulnerability**. Say what is affected (the agent, the app or the plugin, and its version), what an
attacker can do, and how to reproduce it.

There is no fixed response time. A fix goes out as a new release of the part affected, through the catalogue.

## What is in scope

- The install agent (`nbtagent`, `nbtctl`, the launcher and starter scripts): the package checks (signature,
  manifest, hashes, archive paths), the install engine, pairing, and the protocol a phone speaks to it over
  Bluetooth serial (a link the phone opens, or one the car opens to the phone) or the local socket.
- What a channel reaches (`headunit/README.md`, "A package's channels"): a paired phone, or a program on the head
  unit that can open the agent's local socket, opens a channel of an installed package, which reaches that
  package's program, as root, and may start it; a switched channel only once a phone or `nbtctl` has switched it
  on, which the iDrive plugin cannot. So a paired phone that installs a package whose program is a shell, such as
  an SSH server, and switches its channel on, has a root shell on the head unit: that is what such a package is
  for, and the pairing and the package's signature are what keep it to the phone's owner.
- The Android app (`com.nbtapps.store`): the catalogue checks, pairing and the keys it keeps (the pairing keys, its
  own SSH key, and the public keys of other SSH clients that it sends to a head unit), its SSH client's check of a
  head unit's host key against the one the head unit gave over the paired link, the Bluetooth serial links it accepts
  from a head unit, and what it sends to a head unit.
- The iDrive plugin (NBTApp).

## What is not

- The keys in `headunit/tests/keys/`. Their private halves are public on purpose; only tests and the debug
  build's sample catalogue trust them.
- Getting a root shell on a head unit that has no agent yet. The agent is installed from one, and this repo does not
  cover how to get it. Once the agent is there, a root shell through a channel is in scope, above.
- The TCP link of the agent's host build (`--debug-tcp`) and of the app's debug build, which are for the test
  bench only and are not in the builds that go to a car or a phone.

## Versions

Only the newest release of the agent and of the app is fixed. Update through the app to get a fix.
