# Handing a pairing code to NBTApps

A head unit's agent pairs a phone by a code of six digits that it issues on the head unit itself. The user
normally reads the code there and types it into NBTApps. Another app on the phone that already holds such a code
can hand it to NBTApps instead, so the user does not have to type it. NBTApps then asks the user which head unit
to pair with, and on one tap of **Pair** connects to it and sends the code.

This page is the contract between NBTApps and an app that sends a code.

## The request

An activity intent, sent with a plain `startActivity`:

| | |
|---|---|
| Action | `com.nbtapps.store.action.PAIR_HEAD_UNIT` |
| Package | `com.nbtapps.store`, set with `setPackage` |
| Category | none needed (`android.intent.category.DEFAULT` is added by `startActivity`) |

Extras:

| Name | Type | |
|---|---|---|
| `com.nbtapps.store.extra.PAIRING_CODE` | String | Required. Exactly six ASCII digits, `0` to `9`. A String, not a number, because a code may start with zeros. |
| `com.nbtapps.store.extra.EXPIRES_IN_MS` | Long | Optional. How long the code is still good for, in milliseconds, when the intent is sent. NBTApps counts it from when the request arrives, shows a countdown and sends nothing once it has run out. A value above the agent's pairing window (two minutes) is taken as the window; a negative one as zero. Without it there is no countdown, and the agent decides whether the code is still good. |
| `com.nbtapps.store.extra.AGENT_ID` | String | Reserved for the id of the agent that issued the code. Not read yet; a sender may leave it out. |

```kotlin
val intent = Intent("com.nbtapps.store.action.PAIR_HEAD_UNIT")
    .setPackage("com.nbtapps.store")
    .putExtra("com.nbtapps.store.extra.PAIRING_CODE", "042517")
    .putExtra("com.nbtapps.store.extra.EXPIRES_IN_MS", 90_000L)
startActivity(intent)
```

## What happens

- NBTApps shows the code and asks which car to pair with: the only head unit saved, one from a list of several,
  or a car picked there and then from the Bluetooth devices paired with the phone. With no head unit saved, as
  for a user who has never opened NBTApps, the picker is shown at once, with the car the phone is connected to
  picked already where it can be told; the user may first be asked for the Nearby devices permission. Nothing is
  connected before the user taps **Pair**, since connecting replaces whatever link NBTApps already has.
- On **Pair** a car that was picked is saved as a head unit, unless it is saved already, and NBTApps connects to
  it and sends the code. If the phone was already paired with that head unit, it says so and sends nothing. A
  code that has run out by then saves nothing.
- A request never goes through NBTApps' own set-up for a new user. If that set-up is open underneath, it takes
  the car the request paired as its own and goes on from there.
- Under **Advanced** the screen has a box, **Enable autostart**, ticked to begin with. With it ticked, once the
  phone is paired, now or from before, NBTApps sets the head unit up to start the agent with the car: it
  installs the iDrive plugin where the head unit lacks it and switches the start on. The screen stays open and
  shows how that goes, and the user opens NBTApps from it. Last, where the agent finds an app that was installed
  on the head unit by hand, the screen asks the user about it: nothing is moved or removed until the user has
  seen the list and confirmed it. With the box cleared, a successful pairing opens NBTApps' main screen at once.
  The box is the user's: the request cannot set it.
- If the agent refuses the code, or the countdown runs out first, the user is told to get a new code from the app
  that sent this one. The code cannot be changed on that screen.
- A second request while the first is on screen takes its place, unless it carries the same code.

Any app may send a code: NBTApps does not check who sent it. A code is of use only to whoever holds it while the
agent's pairing window is open, and the user still chooses the head unit and taps **Pair**.

## What the sender gets back

Nothing. The intent has no result, and a request that is not as above (no code, a code that is not six digits,
extras that cannot be read) closes at once without a message. While the screen is open for an earlier request,
such a request is ignored, and the screen stays as it was.

The sender learns whether the phone paired from the agent, as it would if the user had typed the code: the agent
closes the pairing window once it has issued the phone its key.

## Telling whether NBTApps takes it

An NBTApps that takes the request has an activity for the action. A sender can resolve the intent above before it
offers the hand-over:

```kotlin
val supported = packageManager.resolveActivity(intent, PackageManager.ResolveInfoFlags.of(0)) != null
```

On Android 11 and later the sender sees NBTApps only if its own manifest declares it in `<queries>`, by the
package or by an `<intent>` with the action. An older NBTApps has no such activity, and `startActivity` throws
`ActivityNotFoundException`; the user then types the code as before.
