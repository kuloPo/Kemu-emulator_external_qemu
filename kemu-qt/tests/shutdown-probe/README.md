# Normal shutdown regression

Build the emulator with `KEMU_GUI_TESTS=ON` and install that runtime in a test
directory. Build the probe with `Build.ps1`, passing the JDK directory, Android
SDK build-tools directory, platform `android.jar`, and an output directory.

Run `../ShutdownTests.ps1` with `-RuntimeRoot`, `-Adb`, `-ProbeApk`, and a new
`-WorkDirectory`. The runner starts only `Kemu.exe`, isolates Android data through
`LOCALAPPDATA`, and refuses to run alongside another engine. Do not put test
artifacts in the runtime distribution directory.

The opt-in GUI hook clicks the real close button or changes resolution and
accepts the real performance restart prompt. Each round writes and then reloads
1 MiB of actual SharedPreferences, without a test-side `sync`. Two consecutive
resolution restarts must preserve the latest data. A final round pauses adbd for
30 seconds: the emulator must remain alive beyond the old 20-second watchdog,
then close successfully when ADB resumes and retain the data on the next boot.
The runner records the guest's `sharedpreferences_skip_fsync` flag for context.

Restore `KEMU_GUI_TESTS=OFF` for the production build.
