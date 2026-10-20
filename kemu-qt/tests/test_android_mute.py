"""Run against a booted emulator: python test_android_mute.py --adb PATH.

The live check toggles media mute twice with the current foreground app intact,
then restores the original state even on failure. Run with each affected app
in the foreground, especially games that consume volume key events.
"""

import argparse
from pathlib import Path
import re
import subprocess
import unittest


SOURCE = Path(__file__).resolve().parents[1] / "src" / "AndroidMute.cpp"
COMMAND = re.search(
    r'kToggleMuteCommand\[\] = R"SH\(shell:(.*?)\)SH";',
    SOURCE.read_text(encoding="utf-8"),
    re.DOTALL,
).group(1)
ADB = ["adb", "-s", "emulator-5554"]


def shell(command):
    return subprocess.run(
        ADB + ["shell", "-T", "-x", command],
        check=True,
        capture_output=True,
        text=True,
        timeout=10,
    ).stdout.replace("\r\n", "\n")


def media_state():
    output = shell("dumpsys audio")
    section = re.search(r"^- STREAM_MUSIC:\n(.*?)(?:\n\n|\Z)", output, re.M | re.S)
    if not section:
        raise AssertionError("Missing media volume state")
    muted = re.search(r"^\s*Muted: (true|false)$", section[1], re.M)
    volumes = re.search(r"^\s*Current: (.+)$", section[1], re.M)
    if not muted or not volumes:
        raise AssertionError("Incomplete media volume state")
    return muted[1], volumes[1]


def foreground():
    output = shell("dumpsys window")
    match = re.search(r"mCurrentFocus=([^\n]+)", output)
    if not match:
        raise AssertionError("Missing foreground window")
    return match[1]


class AndroidMuteTests(unittest.TestCase):
    def run_fake(self, initial, behavior="success"):
        # Shell functions replace only the service calls in this test process.
        # The production command still runs in Android's actual shell.
        prelude = """
fake_state=%s
dumpsys() {
    printf 'Other audio data\\n- STREAM_MUSIC:\\n   Muted: %%s\\n\\n' "$fake_state"
    printf '%%s\\n' '- STREAM_ALARM:' '   Muted: true' ''
}
cmd() {
    [ "$1" = audio ] && [ "$3" = 3 ] || return 1
    case "$2:$fake_state" in
        adj-mute:false) next=true ;;
        adj-unmute:true) next=false ;;
        *) return 1 ;;
    esac
    case %s in
        fail) return 1 ;;
        noop) return 0 ;;
        invalid) fake_state=unknown ;;
        success) fake_state=$next ;;
    esac
}
""" % (initial, behavior)
        return shell(prelude + COMMAND)

    def test_mute_and_unmute(self):
        for state in ("true", "false"):
            with self.subTest(state=state):
                self.assertIn("\n__KEMU_MUTE_EXIT:0\n", self.run_fake(state))

    def test_missing_or_invalid_state_does_not_control_audio(self):
        for state in ("unknown", "''"):
            with self.subTest(state=state):
                prelude = """
dumpsys() { printf '%s\\n' '- STREAM_RING:' '   Muted: false' ''; }
cmd() { echo UNEXPECTED_AUDIO_CHANGE; }
""" if state == "''" else """
dumpsys() { printf '%s\\n' '- STREAM_MUSIC:' '   Muted: unknown' ''; }
cmd() { echo UNEXPECTED_AUDIO_CHANGE; }
"""
                output = shell(prelude + COMMAND)
                self.assertIn("\n__KEMU_MUTE_EXIT:1\n", output)
                self.assertNotIn("UNEXPECTED_AUDIO_CHANGE", output)

    def test_failed_or_ineffective_command_is_not_success(self):
        for behavior in ("fail", "noop", "invalid"):
            for state in ("true", "false"):
                with self.subTest(behavior=behavior, state=state):
                    self.assertIn(
                        "\n__KEMU_MUTE_EXIT:1\n", self.run_fake(state, behavior)
                    )

    def test_live_foreground_toggle_and_restore(self):
        original = media_state()
        window = foreground()
        print("\nForeground:", window, flush=True)
        try:
            self.assertIn("\n__KEMU_MUTE_EXIT:0\n", shell(COMMAND))
            toggled = media_state()
            self.assertEqual(toggled[0], "false" if original[0] == "true" else "true")
            self.assertEqual(toggled[1], original[1], "Saved volume changed")
            self.assertEqual(foreground(), window)
            self.assertIn("\n__KEMU_MUTE_EXIT:0\n", shell(COMMAND))
            self.assertEqual(media_state(), original)
            self.assertEqual(foreground(), window)
        finally:
            if media_state()[0] != original[0]:
                action = "adj-mute" if original[0] == "true" else "adj-unmute"
                shell("cmd audio " + action + " 3")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--serial", default="emulator-5554")
    args, remaining = parser.parse_known_args()
    ADB = [args.adb, "-s", args.serial]
    unittest.main(argv=[__file__] + remaining)
