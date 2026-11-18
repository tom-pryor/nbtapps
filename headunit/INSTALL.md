# Installing NBTApp

This guide sets up NBTApp from nothing: the agent on the car's head unit, the NBTApps app on an Android phone,
and the iDrive plugin, which the app installs. At the end the agent starts with the car and the phone installs
and updates packages over Bluetooth.

> You install at your own risk; see the [README's disclaimer](README.md#disclaimer).

## What you need

- **A BMW with an NBT Evo head unit**, and a root shell on it over SSH. This guide does not cover how to get
  that shell; it starts once you can log in.
- **An Android phone** with Android 13 or later, already paired with the car in the phone's Bluetooth settings
  as for calls or music.
- **A computer** to download two files and to reach the car over SSH.
- **Either a USB stick** (FAT32) **or SSH from the computer** to get the agent's files onto the car.

This guide is the way by hand. There is also an installer at <https://ssh.bavarian-aa.uk/> that puts the agent on
the car by itself. It works on an Android phone or a computer, over an ENET cable, with a USB Ethernet adapter
where the phone or computer has no Ethernet port, and it can switch on the SSH access this guide starts from.
The app's set-up offers both ways. With the agent installed that way, carry on from step 4.

## Downloads

| What | Address |
|---|---|
| The agent, for a first install | <https://catalogue.nbtapps.com/v1/install/nbtapp-agent.zip> |
| The Android app | <https://catalogue.nbtapps.com/v1/app/nbtapps.apk> |

Both addresses always serve the newest release. The agent zip is the agent's own package, the same file the
app installs updates from. The examples below use agent version 1.5.1; where a path contains `1.5.1`, use the
version you downloaded (`cat /var/opt/nbtapp/agent/current` on the car shows it once installed).

## 1. Put the agent's files on the car

Unpack the zip on the computer. It holds a folder `payload` with six files: `hu_install.sh`, `nbtagent`,
`nbtctl`, `nbtlaunch.sh`, `nbtstart.sh` and `version`. The `manifest.json` and `manifest.sig` beside it are not
needed for a first install.

### Option A: USB stick

1. Copy the `payload` folder to the root of the stick.
2. Put the stick in the car's USB port, with the ignition on.

The stick's files are read from `/fs/usb0/payload` in step 2. If `ls /fs/usb0` shows nothing, look in
`ls /mnt` for a folder named `umass…` and use that in place of `/fs/usb0`.

### Option B: over SSH, with no stick

The head unit has no `scp` or `sftp`, so each file is sent through `cat`. In the commands below, `root@CAR`
is however you log in to the head unit; if your SSH client refuses its host key, add
`-o HostKeyAlgorithms=+ssh-rsa`. Run them from the folder that holds `payload`:

```sh
ssh root@CAR 'mkdir -p /net/hu-omap/fs/ram5/payload'
for f in hu_install.sh nbtagent nbtctl nbtlaunch.sh nbtstart.sh version; do
  ssh root@CAR "cat > /net/hu-omap/fs/ram5/payload/$f" < payload/$f
done
```

This logs in seven times. The head unit shuts SSH off for about ten minutes after many logins in a short
time, so don't run the loop over and over.

The files land in `/tmp/payload` on the head unit. `/tmp` is a RAM disk, emptied whenever the head unit
restarts, so carry on with step 2 before the car is switched off.

## 2. Install the agent

1. Log in to the head unit over SSH. The shell opens on one of the head unit's two computers, `hu-jacinto`;
   the agent runs on the other one, so switch to it first:

   ```sh
   on -f /net/hu-omap/ /bin/sh
   ```

2. Check the six files are there:

   ```sh
   ls -l /fs/usb0/payload        # USB stick
   ls -l /tmp/payload            # over SSH
   ```

3. Run the install script. USB sticks don't allow programs to run from them, so the script is handed to
   `ksh`:

   ```sh
   ksh /fs/usb0/payload/hu_install.sh        # USB stick
   ksh /tmp/payload/hu_install.sh            # over SSH
   ```

   Expect:

   ```
   hu_install: nbtagent 1.5.1 ...
   hu_install: version 1.5.1 is in /var/opt/nbtapp/agent/1.5.1, and is the one the launcher starts.
   ```

   The script copies the agent to `/var/opt/nbtapp/agent`, which survives a restart. It starts nothing.

4. Leave the head unit on for a minute afterwards. It cannot flush its writes on demand, so switching off at
   once may lose them.

## 3. Start the agent

```sh
/var/opt/nbtapp/agent/launcher &
```

It keeps running after you log out. Check that it is up:

```sh
cat /tmp/nbtapp/log/agent.log
/var/opt/nbtapp/agent/1.5.1/nbtctl info
```

`nbtctl info` prints the head unit's profile; if it says there is no agent, read the log for the reason.

Until step 6 is done the agent does not start by itself. After any restart of the head unit before then,
start it again with the same command.

## 4. Install the app on the phone

1. On the phone, open <https://catalogue.nbtapps.com/v1/app/nbtapps.apk> in the browser and download it.
2. Open the downloaded file. Android asks you to allow the browser to install unknown apps; allow it, go back
   and install.
3. Google Play Protect may say it has not seen the app before and recommend a scan. Choose for yourself:
   **Scan app** uploads the APK to Google; **More details**, then **Install without scanning**, installs it as
   it is.

The app updates itself from then on: when a new release is published, a card at the top of its catalogue
screen offers it.

## 5. Connect the phone to the car

The first time it is opened, the app opens on its set-up, **Set up your car**, which goes through this step and
the next. Later it is behind **Set up your car** on the catalogue and on the **Head units** page, for as long as
no car is saved.

1. **Is the agent on your car yet?** Tap **Yes, it's installed**.
2. **Which car?** Allow **Nearby devices** when it asks; the app needs it to list the phone's Bluetooth devices.
   The cars paired with the phone are listed, the one the phone is connected to now first and picked already.
   If the car is not among them, look under **Other paired devices**. Pick the car and tap **Connect to**
   with its name. The app calls the head unit by the car's Bluetooth name.
3. The app connects to the agent by itself. If the car has only just started, this can take half a minute.
   While the app is connected, the car maker's own phone features over Bluetooth serial don't work; calls and
   music are not affected. If no agent answers, the page says why and what to check; start the agent (step 3)
   and tap **Try again**.
4. **Pair this phone**: the app asks for a code from the car. The iDrive plugin that would show it is not
   installed yet, so get the code over SSH (**No NBTApp entry on the car's screen?** on that page says the
   same):

   ```sh
   /var/opt/nbtapp/agent/1.5.1/nbtctl pair
   ```

   It prints `code NNNNNN`. Type the six digits into the app within two minutes and tap **Pair**. `nbtctl`
   prints `paired`. Three wrong codes close the window for 30 seconds; run the command again for a new code.

   **Enable autostart**, under **Advanced**, is ticked to begin with. Leave it so: it is how the agent comes to
   start with the car, in step 6. With it cleared the agent has to be started by hand on the car every time,
   and the app says so.

Pairing is done once per phone. **Skip**, on any page of the set-up, leaves it; a car that was saved by then is
on the **Head units** page, and its own page connects and pairs as the set-up does.

## 6. The iDrive plugin, and starting with the car

The plugin is the **NBTApp** entry in iDrive's Media/Radio menu. It starts the agent with the car, and gives
the car's screen **Start the agent** and **Pair a phone**.

With **Enable autostart** ticked, the app sets this up by itself once the phone is paired. The set-up's last
page, **Start with the car**, lists the stages and ticks each one off:

1. **Checking the head unit…**: the agent looks for an app that was installed on the car by hand. Nothing is
   asked yet.
2. **Getting the iDrive plugin…**: the app downloads the plugin (`org.nbtapp.plugin`) from the catalogue,
   which needs the phone's internet connection.
3. **Installing the iDrive plugin…**: the agent installs it.
4. **Switching on…**, and then **The agent now starts with the car.**
5. Where the check found an app that was installed by hand, the page asks about it now, last: see
   [An app that was installed by hand](#an-app-that-was-installed-by-hand). On a car without one there is no
   question.
6. iDrive loads the plugin only when it starts, so the page offers **Restart iDrive now**. On NBT Evo this
   restarts the whole head unit, and the agent stops with it: the app loses its connection, which is
   expected. The set-up ends on a page that says what was set up.
7. Once iDrive is back, wait 15 seconds for the plugin to start the agent, and tap **Connect to the agent**
   on the head unit's page. The plugin now shows as installed.

The set-up goes on if you leave that page; the head unit's page shows it as a card, **Setting up**, where
**Show progress** opens the plugin's job. If it stops, the page or the card says why and offers **Retry**,
which carries on from where it stopped. A head unit that has the plugin already is only switched on: the
plugin is not updated for this.

If you copied the plugin onto this car by hand before, the install fails with `stock-mismatch`. Move the old
folder aside once and tap **Retry**. In a shell on `hu-omap` (step 2):

```sh
/bin/mount -u -o rw /fs/sda0
mv /opt/hmi/ID5/data/ro/Plugins/NbtApp /opt/NbtApp.old
/bin/mount -u -o ro /fs/sda0
```

### An app that was installed by hand

With an agent from 1.4.0, the app has the head unit checked once, the first time it connects after pairing,
for what an app that was installed by hand, outside NBTApp, left there:

| What the app shows | What it is | Its fix |
|---|---|---|
| **An app that was installed by hand** | Files in `/opt/aa` and `/var/opt/aa`, and that app's own iDrive plugin | **Remove that app** |
| **The system partition was left writable** | It was remounted by hand and not put back | **Make it read-only again**. Nothing is removed |

**Remove that app** does this, in this order:

1. It stops the programs that run from `/opt/aa`.
2. It moves that app's plugin folder out of iDrive's plugin folder into `/opt/aa`, under its own name with
   `.old` at the end (`.nbtappsold` where that name is taken), and leaves it there. The folder is moved and
   not deleted, because iDrive may have the plugin loaded. From there iDrive no longer loads it.
3. It removes the other files in `/opt/aa` and `/var/opt/aa`, one by one.

The system partition is writable for the fix only, and read-only again afterwards. What that app changed in
the packet filter's configuration stays as it is. To have the plugin back, move its folder back by hand.

The set-up asks about the app last, once the agent starts with the car. It says how many folders would be
moved and how many paths removed:

- **Remove that app…** opens the list: the folder that is moved, with where it goes, and every path that is
  removed. Nothing changes until you confirm there, and only what is on that list is touched. If the files
  changed in the meantime, nothing is done and the list is shown again as it is now.
- **Skip** leaves everything as it is. The set-up is done either way.

A fix needs iDrive to restart, as the plugin does. The one **Restart iDrive now** at the end of the set-up
covers both.

The same is on the head unit's page under **Checks**, with or without the set-up: each thing found, what
would be moved and removed, its fix, and **Dismiss** to put it aside. **Check again** runs the checks once
more and brings back what was put aside.

Over SSH, `nbtctl checks` prints a line for each check, `nbtctl checks --list` what each one would move and
remove, and `nbtctl checks fix aa-fix` (or `system-writable`) runs that check's fix on what it finds:

```sh
/var/opt/nbtapp/agent/1.5.1/nbtctl checks --list
```

## 7. The same by hand

Skip this step if step 6 ended with **The agent now starts with the car.** It is for a phone that was paired
with **Enable autostart**, under **Advanced**, cleared.

1. In the app's catalogue, open **NBTApp iDrive plugin** (`org.nbtapp.plugin`) and install it.
2. On the head unit's page in the app, switch on **Start the agent when the car starts**. If the switch
   cannot be turned on, the card says why. Usually the plugin is missing or too old, and the card links to
   it.
3. Restart iDrive from the job's page, **Restart iDrive now**, and connect again as in step 6.

If iDrive was restarted before the switch was on, nothing starts the agent afterwards. Start it from the
car's screen: **NBTApp**, then **Start the agent**, then **Start it now**, and wait for **Started. A phone
can connect now**. Starting it again over SSH, as in step 3, does the same. Then connect and switch on.

The same switch over SSH:

```sh
/var/opt/nbtapp/agent/1.5.1/nbtctl autostart on
```

From the next start of the head unit, the plugin starts the agent about 15 seconds after iDrive comes up.

## 8. Check it works

Restart the head unit: hold its power button for 30 seconds, or switch the car off until the head unit has
shut down and start it again. After iDrive is up, wait 15 seconds and tap **Connect to the agent** in the app.
It should connect without any step on the car.

From now on:

- **Packages** are installed, updated and removed from the app's catalogue.
- **The agent updates itself**: when a newer agent is published, the app offers it on the head unit's page.
  The new version gets one trial start; if it fails, the car goes back to the version before.
- **The app updates itself** from the card on its catalogue screen.

## When something goes wrong

| What you see | What to do |
|---|---|
| `hu_install: version 1.5.1 is there already` | The script never writes over an installed version. Stop the agent (`slay nbtagent`), remove that version (`rm -rf /var/opt/nbtapp/agent/1.5.1`) and run the script again |
| `hu_install: an update of the agent was begun and is not finished` | Start the agent (step 3) and let it finish the update, then try again |
| The app cannot connect | Check the agent runs: **NBTApp**, **Start the agent** on the car's screen, or `nbtctl info` over SSH. Read `/tmp/nbtapp/log/agent.log` |
| **Start with the car**, or the **Setting up** card, stopped with a reason | Put right what it names (the phone's internet connection, the link to the head unit) and tap **Retry**. Or do step 7 by hand |
| The app says a thing on the head unit **needs a look** | The checks found something that was put onto the car by hand. **Show** opens the head unit's page, which lists it under **Checks** with its fix; see step 6 |
| The agent no longer starts with the car | After three starts in a row in which the agent did not keep running for five minutes, the plugin stops trying. The card in the app says so; tap **Have the car try again**. Over SSH: `rm /var/opt/nbtapp/agent/autostart.boots` |
| The phone connects but nothing arrives | Bytes left over from an earlier link can hold up the next ones. On the car's screen open **NBTApp**, **Reset Bluetooth link** and press **Reset now**, then tap **Connect** in the phone app; this needs no SSH. If the screen says the agent cannot do this, the agent is too old for any of these ways: restart the head unit, and update the agent from the app. Over SSH, `nbtctl serial reset` does the same, and the agent's log says what came of it. Whenever the app does reach the agent, **Reset Bluetooth link** under **Troubleshooting** on the head unit's page in the app does the same, and connects again by itself. If links still carry nothing, restart the head unit |
| **Pair a phone** on the car's screen shows no code | Get one over SSH: `nbtctl pair`, as in step 5 |

The agent's log is `/tmp/nbtapp/log/agent.log`, with the two starts before it as `agent.log.1` and
`agent.log.2`. A restart of the head unit empties them. The plugin logs to the system log:
`sloginfo | grep NbtApp`.

### If iDrive does not start

The iDrive plugin runs inside the program that draws iDrive. If iDrive does not come back after the plugin was
installed or updated, switch NBTApp off over SSH, in a shell on `hu-omap` (step 2):

```sh
echo off > /var/opt/nbtapp/disable
```

Then restart the head unit: hold its power button for 30 seconds. With that file there, the plugin gives iDrive
no menu entry and starts no agent.

If iDrive still does not start, move the plugin out of iDrive's plugin folder and restart again:

```sh
/bin/mount -u -o rw /fs/sda0
mv /opt/hmi/ID5/data/ro/Plugins/NbtApp /opt/NbtApp.off
/bin/mount -u -o ro /fs/sda0
```

The app still lists the plugin as installed. Put the folder back before you update or remove it from the app:

```sh
/bin/mount -u -o rw /fs/sda0
mv /opt/NbtApp.off /opt/hmi/ID5/data/ro/Plugins/NbtApp
/bin/mount -u -o ro /fs/sda0
```

To switch NBTApp on again, remove the file and restart the head unit:

```sh
rm /var/opt/nbtapp/disable
```

## Removing NBTApp

1. In the app, uninstall every package from the head unit's page, the iDrive plugin last. The agent removes
   what it installed and restarts iDrive.
2. Over SSH, stop the agent and remove it with its state (paired phones and records of installed packages):

   ```sh
   slay nbtagent
   rm -rf /var/opt/nbtapp
   ```

3. Uninstall the app from the phone.

To switch NBTApp off without removing anything, make the file `/var/opt/nbtapp/disable` and restart the head
unit; the plugin then shows no menu and starts nothing. The head unit has no `touch`, so:

```sh
echo off > /var/opt/nbtapp/disable
```

Remove the file and restart to switch it back on.

## More

The [README](README.md) has the detail behind each step: [the agent on the head
unit](README.md#the-agent-on-the-head-unit), [starting with the head unit](README.md#starting-with-the-head-unit),
[the plugin's package](README.md#the-plugins-package) and [how the agent updates
itself](README.md#the-agent-updates-itself).
