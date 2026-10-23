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
