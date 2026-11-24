# Changelog

What changed in each release of the NBTApps app, the agent and the iDrive plugin.
Generated from the catalogue's release notes.

## 2026-11-24

### App 0.10.0
- The app reaches an agent whose Bluetooth service the car does not advertise to phones: it then asks for the car's link through the car's Serial Port service.
- With agent 1.8.0 the app asks for the car's link through the car's dial-up networking service first, which cannot disturb another app's Bluetooth link to the car, such as wireless Android Auto's.
- A connect that may have run into another app's link to the car stops there and says so, and can be tried again a few seconds later.
- A first connect to a head unit asks for the car's link before the phone opens a link of its own.
- Agent 1.8.0 needs this version.

### Agent 1.8.0
- Wireless Android Auto could fail to connect while the agent was running: the agent could take the last place in the car's Bluetooth that a phone can find.
- The agent now keeps such a place for every package that declares a Bluetooth serial service, such as wireless Android Auto, and gives it up when that package's program starts.
- The agent waits until the car's Bluetooth has been on for 10 seconds before it registers its services, so the car's own come first.
- These changes take effect from the next start of the head unit after the update.
- The open app can ask for the car's link through the car's dial-up networking service, which cannot disturb another app's Bluetooth link to the car, such as wireless Android Auto's.
- The agent keeps that service switched on; nbtagent --knock-dialup off turns this off.
- Needs app 0.10.0 or later. An agent older than 1.6.0 updates to 1.6.0 first.

## 2026-11-23

### App 0.9.0
- With the SSH access package installed, the head unit's page has a Debug card with a terminal: a root shell on the head unit.
- SSH from other apps: a port on the phone for another SSH client, and for other devices on the network if you allow them.
- Connecting to another head unit asks first when that would close a terminal or switch the SSH port off.
- The terminal and the SSH port need agent 1.7.0 or later.

### Agent 1.7.0
- Channels: a package can have a program on the head unit that the NBTApps app talks to through the agent, started when it is first needed.
- A channel can have a switch, which is off until it is switched on and off again after the head unit has powered down.
- A job that installs, updates or removes a package closes that package's channels first, and other channels wait until the job has ended.
- nbtctl channel opens a channel from the head unit's command line.
- Removing a file is tried again when the head unit interrupts it.
- The SSH access package needs this agent.
- Needs app 0.8.2 or later. An agent older than 1.6.0 updates to 1.6.0 first.

### org.nbtapp.ssh 1.0.0
- First release.
- A root shell on the head unit over SSH: a terminal in the app, and a port on the phone for other SSH clients, with scp and sftp.
- SSH access is switched on from the Debug card on the head unit's page, and is off again after the head unit has powered down.
- Needs agent 1.7.0 or later and app 0.9.0 or later.

## 2026-11-22

### App 0.8.2
- With agent 1.6.2 or later, the app asks the car to connect when it is opened, and the car no longer dials the phone in the background.
- This stops the Bluetooth media audio drops and the trouble with wireless Android Auto that those dials could cause.
- A connect that fails says when the car is not in reach, and when the car has no media audio connection to the phone, which the car needs before it takes the app's link.
- After an agent update the app keeps looking for the restarted agent, and says so if it hasn't come back.
- A package's page has Uninstall, icons on its buttons, and shorter lists of what is new and of versions.
- Fixes for connects that could wait without end.

### Agent 1.6.2
- The car now dials the phone only when the app is opened and asks for it.
- The car no longer dials by itself in the background, which stops the failed dials that could make the car drop the phone's Bluetooth media audio and refuse wireless Android Auto.
- A link the phone opens for another program, such as wireless Android Auto, is handed to that program whole.
- After a dial that does fail, the agent keeps it from counting against the phone's connections to the car.
- A dial that gets no answer no longer blocks later dials.
- The agent can be stopped until the head unit next powers down, from the iDrive menu with plugin 1.3.0 or with nbtctl stop.
- An app installed by hand is stopped before its files are removed.
- Needs app 0.8.2 or later. An agent older than 1.6.0 updates to 1.6.0 first.

### Plugin 1.3.0
- A new row, Stop the agent, stops the agent until the head unit has powered down
- After such a stop the agent is not started again by an iDrive restart; Start the agent starts it
- The new row needs agent 1.6.2 or later

## 2026-11-19

### App 0.8.0
- With agent 1.6.0 or later, the car connects to the open app by itself, so wireless Android Auto can stay on.
- A connect that fails says why, with diagnostics to copy.
- A connect no longer waits without end for an answer from the head unit.
- The app says so when the phone's Bluetooth was switched off.
- A package made for a later app, or in a later format, says to update the app.

### Agent 1.6.0
- The car now connects to the open app by itself, so wireless Android Auto can stay on.
- Bluetooth serial checks its registration and recovers by itself after the car's Bluetooth is switched off and on.
- A package can place a plugin in the writable plugin folder, and can name the app version it needs.

## 2026-11-18

### Agent 1.5.1
- Starts on a head unit that never had a plugin: a missing Plugins folder no longer stops the agent.
- The first plugin installed creates the Plugins folder, and it is removed again when the last plugin goes.

## 2026-11-17

### App 0.7.0
- With agent 1.5.0, a package's settings: change the options it lists from the head unit's page or the package's catalogue page
- A package whose settings are of a later format still downloads, and an option the app can't show yet can still be reset
- Logs a package keeps in /dev/shmem can be pulled, and a log in a folder the app doesn't know is skipped instead of refusing the package
- A link that ends while the app is waiting to connect again no longer stops it reconnecting by itself
- The autostart set-up takes Retry and Hide as soon as it shows how it ended

### Agent 1.5.0
- A package's settings: the phone app shows and changes the options its manifest lists, and nbtctl settings, set and unset do the same
- A package can name its logs in /dev/shmem, such as Android Auto's RAM logs, and a log in a folder this agent doesn't know is skipped instead of refusing the package
- A package that needs a newer agent is refused as needing a newer agent, not as a bad manifest

## 2026-11-16

### App 0.6.0
- A set-up for a new user: pick the car from the phone's paired devices, connect, pair and have the agent start with the car
- For a car with no agent yet, the set-up points to the installer and to the install guide
- A code another app hands over picks the car from the phone's paired devices
- The Bluetooth link is ended while the app is in the background and made again when you come back
- With agent 1.4.1, Reset Bluetooth link on the head unit's page, for a phone that connects and gets nothing

### App 0.5.0
- Pre-release versions of a package can be switched on, package by package
- Another app can hand over the pairing code, so it need not be typed
- Pairing can set up the start with the car: it installs the iDrive plugin and switches autostart on
- With agent 1.4.0, checks the head unit for an app installed by hand and offers to remove it
- Connect and Disconnect work while the app waits to reconnect after an agent update

### Agent 1.4.1
- Reads a Bluetooth link until the car ends it and passes over bytes an earlier link left, for a phone that connected and got nothing
- Reset Bluetooth link: has the car end the phone's link, from the car's screen, the app or nbtctl serial reset

### Plugin 1.2.0
- A new screen, Reset Bluetooth link, for a phone that connects and gets nothing
- Forget all phones asks again after it was left while it waited

## 2026-11-15

### Agent 1.4.0
- Checks the head unit for an app that was installed by hand, and can move its plugin aside and remove the rest once you confirm
- Finds a system partition that was left writable, and can put it back to read-only

## 2026-11-13

### App 0.4.1
- Restarting the head unit from the app no longer reports a failure when it worked
- Getting logs uses the choices just made, such as only the end of each file

### App 0.4.0
- Get logs: an app's or the agent's log files from the head unit, in a ZIP to share
- Getting logs needs the agent 1.3.0 or later on the head unit

### App 0.3.0
- Lists community packages: packages from outside developers, reviewed and signed for the catalogue
- A community package installs on a head unit whose agent is 1.2.0 or later

### Agent 1.3.0
- Sends an app's log files to the phone, for the app to share

### Agent 1.2.0
- Can install apps whose authors build and sign them, with the catalogue's signature for each one

## 2026-11-12

### App 0.2.1
- No changes beyond the version: the first update made through the app itself

### App 0.2.0
- Updates itself: a newer release shows as a card at the top of the catalogue
- Starting the agent with the car can be switched on and off from the phone

### Agent 1.1.1
- Starting the agent with the car can be switched on and off from the phone app, once the iDrive plugin is installed
- Each start's log says which starter is in place

### Agent 1.1.0
- An update now brings the agent's launcher, which is tried together with the new version
- A new launcher that does not run on the car fails its trial, and the version before comes back
- Cannot be reached by an update from 1.0.x: such a car is installed again from a USB stick

### Agent 1.0.3
- Sets up its Bluetooth link to the phone again by itself when the car's Bluetooth was not ready for it
- Works with an iDrive plugin that starts the agent with the head unit
- The package also holds what a first install from a USB stick needs

### Agent 1.0.2
- Tells the phone before it ends a Bluetooth link, and waits for the phone to end it

### Agent 1.0.1
- Works on the head unit itself

### Plugin 1.1.1
- The menu's screens answer: Start the agent, Pair a phone and Forget all phones did nothing in 1.0.0 and 1.1.0

### Plugin 1.1.0
- A Start the agent row in the iDrive menu starts the agent on a head unit with no console

### Plugin 1.0.0
- First release as a package

## 2026-11-11

### Agent 1.0.0
- First release
