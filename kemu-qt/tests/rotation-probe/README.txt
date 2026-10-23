Rotation regression checks

Run kemu-rotation-tests for primary-display parsing and portrait window bounds.

For live tests, build this APK with build.ps1 -Sdk <Android SDK> -Jdk <JDK> -Output <scratch directory>.
Install rotation-probe.apk with adb install. The fixture uses its own package
org.kemu.test.rotation and does not require clearing any user application data.

Build the emulator with KEMU_GUI_TESTS=ON. Set KEMU_TEST_ADB to the absolute
adb.exe path and KEMU_ROTATION_TEST_RESULTS to an output JSON path before
launching Kemu. Do not also enable KEMU_GUI_TEST_RESULTS. The opt-in checks run
after boot and exercise four directions, repeated maximized rotation, restoring
both portrait and landscape geometry, minimized rotation, and a touch in the
top-left quarter. All 15 steps must pass. Inspect passed and checks in the JSON.

The test assumes a natural 1920x1080 landscape display. Uninstall the fixture
with adb uninstall org.kemu.test.rotation, then rebuild with KEMU_GUI_TESTS=OFF
before distributing the emulator. No test hooks are enabled in release builds.

For the sensor rotation button, use KEMU_SENSOR_ROTATION_TEST_RESULTS instead
of KEMU_ROTATION_TEST_RESULTS. Optionally set KEMU_SENSOR_ROTATION_TEST_APK to
the built APK path to install it at the start of the checks. KEMU_TEST_ADB is
still required; this suite uses the running instance's serial number.

The 17 checks click the real sidebar button and read accelerometer, gyroscope,
and display events inside Android. They cover repeated 0/90-degree toggles with
full sensor rotation, system rotation lock, fixed portrait, fixed landscape,
and three rapid clicks. They verify every requested pose and button icon,
including the initial icon selected by the configured display orientation.
Run this suite with both landscape and portrait resolution settings.
They also verify that Android rotation settings are unchanged, the host display
and touch coordinates follow Android, and the button remains visible. The suite
restores the original rotation settings and physical pose when it finishes.
