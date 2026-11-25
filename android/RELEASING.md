# Releasing the app

GitHub Actions builds a release and the maintainer signs it, on their own machine. The release key is never on
GitHub, nor seen by Gradle: the release build is always unsigned, and `tools/sign.sh` signs it with `apksigner`,
which asks for the key's password.

The build is reproducible: CI's APK and one built here from the same commit are the same file, byte for byte. A
published APK is that file with the signature added, so anyone can check that it is CI's build of the tagged
commit (`tools/compare_apks.py`).

## A release, step by step

1. Raise `appVersion` in `app/build.gradle.kts` (major.minor.patch; a phone takes an update only when the version
   rises) and merge it to main. The version code follows from it.
2. Tag the merged commit and push the tag:

   ```
   git tag android-v0.4.1 <commit>
   git push origin android-v0.4.1
   ```

3. The workflow **Android release** runs the tests with the real agent and the check of the app's list of
   licences (`checkLicences`), builds the unsigned APK (`tools/build_release.sh`), and makes a draft release of
   the tag holding `nbtapps-<version>-unsigned.apk` and its SHA-256. It refuses a tag that is not `android-v` +
   `appVersion`.
4. Optionally, build the same commit here and keep it for the comparison:

   ```
   git switch --detach android-v0.4.1
   tools/build_release.sh
   ```

5. Write the release's notes, what is new for the user, in a file outside the repo, one note on each line. They
   are what the release on GitHub, the app and the changelog say of it, so they keep to the catalogue's rule for
   notes: no blank line, none over 200 characters, no control character. The app shows them to everyone who
   updates to the release: in a sheet the first time they open the app after the update, and under Settings,
   What's new. So they are written for the user, not for a developer. `tools/sign.sh` publishes no release on
   GitHub without them; a release that the catalogue records with no notes, as one signed with `--from` and no
   `--notes` would be, shows no sheet.
6. Sign and publish:

   ```
   tools/sign.sh 0.4.1 --notes ~/notes-0.4.1.txt --compare release/nbtapps-0.4.1-unsigned.apk   # or without --compare
   ```

   It checks the notes first, before it looks at the key or GitHub, and refuses a file that breaks the rule. It
   downloads the draft's APK and checks it against its SHA-256, checks the APK (this app, that version, not
   debuggable, aligned, unsigned), with `--compare` requires your build to be the same bytes, signs it (schemes v2
   and v3, the entries left as they are), and checks the signature: it verifies, one signer, the release
   certificate. It leaves `release/nbtapps-<version>.apk`, `release/release.json` and a copy of the notes,
   `release/notes.txt`. Then it shows the text the release will have on GitHub: the notes under "What's new", a
   link to the changelog, and below a line the build and what was signed. It asks before it adds the APK to the
   draft and publishes it with that text. Nothing on GitHub changes until you say yes.
7. Record the release in the catalogue repo from `release/release.json`, which takes the notes from
   `release/notes.txt` beside it (`tools/nbtcat.py set-app`). Write the changelog into this repo from there
   (`tools/nbtcat.py changelog --out ../nbtapps-mono/CHANGELOG.md`) and commit it here, then publish the catalogue.
   Phones see the release the next time they fetch the catalogue: when the app starts, when it comes to the front
   or a package's page is opened with a copy that is at least 30 minutes old, and when the list is pulled down.

`tools/sign.sh VERSION --from FILE` signs an unsigned APK you have here and leaves GitHub out: for a trial, or if
GitHub is not to be used. `--notes` is optional there; without it no `release/notes.txt` is left, and one from an
earlier run is removed.

[CHANGELOG.md](../CHANGELOG.md) at the top of this repo holds every release of the app, the agent and the plugin.
It is made in the catalogue repo from the notes recorded there, and never edited by hand.

## A dry run

**Android release** can be run by hand (the Actions tab, or `gh workflow run android-release.yml --ref BRANCH`):
it runs the tests and the licence check, builds and checks the APK, and keeps it with the run for two weeks,
without a release. A pull request that changes the workflow or the build's scripts runs it the same way.

## The key

The key is made once, and every release is signed with it:

```
keytool -genkeypair -keystore ~/.config/nbtapps/keys/android-release.jks -alias nbtapps \
    -keyalg RSA -keysize 4096 -validity 36500 -dname "CN=NBTApps"
```

`~/.config/nbtapps/keys/android-release.properties` (or the file `NBTAPPS_SIGNING` names) says where it is:

```
storeFile=android-release.jks
keyAlias=nbtapps
```

`storeFile` is a path from the properties file's folder, or a whole path. The file holds no password:
`tools/sign.sh` refuses one there, and `apksigner` asks for it. The store is PKCS12, which is encrypted with that
password.

The certificate's SHA-256 is `843dd6519def36860dfff183634424753b00a059846d6e2662ba608bff04d587`; `tools/sign.sh`
refuses to leave an APK signed with any other.

Keep a copy of the key, and its password, somewhere else. Android takes an update only from the key the installed
app was signed with: if the key is lost, no installed app can be updated again, and users have to uninstall and
install anew.
