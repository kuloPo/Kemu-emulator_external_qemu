# Rotation observer

`AndroidRotation` runs this small Java helper through the emulator's internal
ADB connection. The helper registers `IWindowManager.watchRotation` for display
0, publishes the initial rotation, and then emits one line per change:

```
KEMU_ROTATION_V1 0
```

Values 0 through 3 are Android's display rotations. The host applies the inverse
skin transform. The observer follows application orientation requests without
changing the device's sensors or rotation settings.

The stream stays open while the GUI is active. Closing its stdin terminates the
helper and releases the Binder subscription. A window-service death also ends
the helper. The host cancels startup after five seconds without an initial
event, reconnects with a one-to-ten-second backoff, and resets the delay after
a valid event. Successful subscriptions have no periodic polling or timeout.

## Build

CMake compiles the helper from source using the existing prebuilt JDK 17,
Android 36 API jar, and D8 36.1.0, then embeds the resulting dex in the GUI.
`IRotationWatcher.java` only supplies compile-time declarations; its classes
are excluded from the dex so Android supplies the real Binder implementation.
No APK installation, Android image change, or external `adb.exe` is required
for rotation detection.

The CMake cache variables `KEMU_ROTATION_JAVA`, `KEMU_ROTATION_JAVAC`,
`KEMU_ROTATION_D8`, and `KEMU_ROTATION_ANDROID_JAR` override the tool paths.
The generated header remains in the build directory. The host verifies and
caches the dex under `/data/local/tmp/kemu-rotation-<uid>/<sha256>.dex`, installing
it atomically and read-only. Separate caches support ADB root/unroot transitions.

## Validation

Build and run `kemu-rotation-tests` for fragmented events, duplicate suppression,
malformed or oversized lines, reconnection state, and window sizing.

For live rotation and touch-coordinate checks, build with `KEMU_GUI_TESTS=ON`,
install the APK built by `../tests/rotation-probe/build.ps1`, and launch Kemu with
`KEMU_TEST_ADB` and `KEMU_ROTATION_TEST_RESULTS` set. The existing GUI checks cover
all four orientations, maximization, minimization, and restored window geometry.
Also verify that restarting adbd creates one replacement observer, and closing
the emulator completes without waiting for rotation input.
