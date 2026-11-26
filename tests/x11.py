"""Exercise the normal CLI, script, and editor paths against an isolated X server."""

import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

app = Path(sys.argv[1]).resolve()
multi = "--multi" in sys.argv
assert os.environ.get("DISPLAY"), "Run under Xvfb or a private dummy Xorg."


def run(*args, code=0):
    result = subprocess.run(args, text=True, capture_output=True, timeout=10)
    assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
    return result.stdout


def call(*args, code=0):
    return run(app, *args, code=code)


def wait_for(predicate, seconds=4):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(0.05)
    raise AssertionError(f"Timed out: {predicate!r}")


def state():
    return json.loads(call("status", "--json"))


def window(name):
    result = subprocess.run(["xdotool", "search", "--name", f"^{name}$"],
                            text=True, capture_output=True)
    return result.stdout.splitlines()[0] if result.stdout else None


def click(wid, x, y):
    run("xdotool", "mousemove", "--window", wid, str(x), str(y), "click", "1")
    time.sleep(0.2)


def hook_events(root):
    path = root / "events"
    return path.read_text().splitlines() if path.exists() else []


def layout(root):
    status = state()
    outputs = status["outputs"]
    assert [out["port"] for out in outputs] == ["DUMMY0", "DUMMY1", "DUMMY2"], outputs
    call("save", "Office dock", "--auto")
    call("layout", "y", "--dry-run")
    call("layout", "y")
    y = state()
    assert y["confirmation_seconds"] > 0
    lower, *upper = y["outputs"]
    top_height = max(out["mode"]["height"] for out in upper)
    top_left = min(out["position"][0] for out in upper)
    top_width = sum(out["mode"]["width"] for out in upper)
    assert lower["position"][1] == top_height
    assert abs(2 * (lower["position"][0] - top_left) + lower["mode"]["width"] - top_width) <= 1
    for out in upper:
        assert out["position"][1] == (top_height - out["mode"]["height"]) // 2
    wait_for(lambda: hook_events(root) == ["apply:"])
    assert state()["confirmation_seconds"] > 0
    call("keep")
    time.sleep(0.45)
    assert hook_events(root) == ["apply:"]
    call("layout", "mirror")
    mirrored = state()
    assert all(out["position"] == [0, 0] for out in mirrored["outputs"])
    wait_for(lambda: hook_events(root) == ["apply:", "apply:"])
    wait_for(lambda: not state()["confirmation_seconds"], seconds=7)
    assert state()["outputs"] == y["outputs"]
    wait_for(lambda: hook_events(root) == ["apply:", "apply:", "revert:"])
    call("layout", "mirror", "--yes")
    call("layout", "extend", "--yes")
    assert state()["outputs"] == y["outputs"]
    call("output", "DUMMY1", "off")
    assert not state()["outputs"][1]["enabled"]
    call("revert")
    assert state()["outputs"][1]["enabled"]
    time.sleep(0.65)
    before = len(hook_events(root))

    run("xrandr", "--output", "DUMMY1", "--off", "--set", "non-desktop", "1")
    wait_for(lambda: len(state()["outputs"]) == 2)
    wait_for(lambda: len(hook_events(root)) == before + 1)
    run("xrandr", "--output", "DUMMY1", "--set", "non-desktop", "0")
    wait_for(lambda: state()["status"] == "Automatic preset: Office dock")
    final = state()
    assert [out["position"] for out in final["outputs"]] == [out["position"] for out in outputs]
    wait_for(lambda: len(hook_events(root)) == before + 2)
    time.sleep(0.5)
    assert len(hook_events(root)) == before + 2
    assert hook_events(root)[-2:] == ["undock:", "dock:Office dock"]


def check_alignment():
    for axis, modes in ((1, ("row-top", "row-center", "row")), (0, ("column-left", "column", "column-right"))):
        dimension = ("width", "height")[axis]
        for divisor, kind in zip((0, 2, 1), modes):
            call("layout", kind, "--yes")
            outputs = state()["outputs"]
            widest = max(out["mode"][dimension] for out in outputs)
            assert len({out["mode"][dimension] for out in outputs}) > 1
            for out in outputs:
                spare = widest - out["mode"][dimension]
                assert out["position"][axis] == (spare // divisor if divisor else 0)
    call("preset", "Office dock", "--yes")


with tempfile.TemporaryDirectory(prefix="undocked-test-") as temp:
    root = Path(temp)
    config = root / "config" / "undocked"
    config.mkdir(parents=True)
    runtime = root / "runtime"
    runtime.mkdir(mode=0o700)
    (root / "undocked").symlink_to(app)
    os.environ.update(XDG_CONFIG_HOME=str(config.parent), XDG_RUNTIME_DIR=str(runtime),
                      PATH=f"{root}:{os.environ['PATH']}")
    (config / "config.ini").write_text(
        "[undocked]\nsettle_ms = 180\nconfirm_seconds = 5\nidentify_seconds = 1\ntray = true\n"
        f"after_change = printf '%s:%s\\n' \"$UNDOCKED_REASON\" \"$UNDOCKED_PRESET\" >> '{root / 'events'}'\n"
    )
    daemon = subprocess.Popen([app, "daemon"])
    editor = None
    try:
        wait_for(lambda: any(path.is_socket() for path in runtime.iterdir()))
        wait_for(lambda: "No automatic preset" in state()["status"])
        initial = state()
        assert initial["outputs"] and initial["version"] == 1
        output = initial["outputs"][0]["port"]
        call("save", "Portable layout", "--auto")
        script = config / "presets" / "Portable layout.sh"
        assert script.stat().st_mode & 0o111
        assert "# Undocked preset 1" in script.read_text()
        run(script, "--dry-run")
        call("save", "Portable layout", code=1)
        assert json.loads(call("presets", "--json"))["presets"][0]["matches"]
        call("auto", "Portable layout", "off")
        assert not json.loads(call("presets", "--json"))["presets"][0]["autoapply"]
        if multi:
            layout(root)
            check_alignment()
        else:
            call("output", output, "off", "--yes", code=1)
            assert state()["outputs"][0]["enabled"]

        call("identify")
        wait_for(lambda: window("Undocked Identify"))
        wait_for(lambda: not window("Undocked Identify"))
        editor = subprocess.Popen([app])
        wid = wait_for(lambda: window("Undocked"))
        assert "_NET_WM_WINDOW_TYPE_DIALOG" in run("xprop", "-id", wid, "_NET_WM_WINDOW_TYPE")
        assert '"Undocked"' in run("xprop", "-id", wid, "WM_CLASS")
        time.sleep(0.3)
        if multi:
            original = state()["outputs"]
            # Quick Row is middle aligned. More lists it alongside Top and Bottom.
            click(wid, 151, 464)
            click(wid, 697, 464)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            preview = state()["outputs"]
            assert preview[0]["position"][1] == (max(out["mode"]["height"] for out in preview) -
                                                  preview[0]["mode"]["height"]) // 2
            click(wid, 611, 464)
            wait_for(lambda: state()["outputs"] == original)
            click(wid, 415, 464)
            click(wid, 250, 117)
            click(wid, 697, 464)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            assert state()["outputs"] == preview
            click(wid, 611, 464)
            wait_for(lambda: state()["outputs"] == original)
            click(wid, 415, 464)
            click(wid, 410, 117)
            click(wid, 697, 464)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            bottom = max(out["mode"]["height"] for out in original)
            assert state()["outputs"][0]["position"][1] == bottom - original[0]["mode"]["height"]
            click(wid, 611, 464)
            wait_for(lambda: state()["outputs"] == original)
            run("xdotool", "mousemove", "--window", wid, "147", "233", "mousedown", "1",
                "sleep", "0.2", "mousemove", "--window", wid, "195", "277",
                "sleep", "0.2", "mouseup", "1")
            time.sleep(0.2)
            click(wid, 697, 464)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            assert state()["outputs"] != original
            click(wid, 611, 464)
            wait_for(lambda: state()["outputs"] == original)
            click(wid, 147, 233)
            click(wid, 615, 184)
            active = original[0]["mode"]
            modes = original[0]["modes"]
            run("xdotool", "click", "--repeat", str(len(modes)), "--delay", "5", "4")
            first_mode_is_active = all(modes[0][key] == value for key, value in active.items())
            click(wid, 210, 106 + 33 * first_mode_is_active)
            click(wid, 445, 464)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            assert state()["outputs"][0]["mode"] != active
            click(wid, 350, 464)
            wait_for(lambda: state()["outputs"] == original)
            click(wid, 728, 34)
        click(wid, 692, 34)
        # The active preset remains loadable and the header opens the list again.
        click(wid, 649, 96)
        click(wid, 692, 34)
        click(wid, 676, 34)
        run("xdotool", "windowfocus", wid)
        run("xdotool", "type", "--delay", "10", "Editor preset")
        run("xdotool", "key", "Return")
        created = config / "presets" / "Editor-preset.sh"
        wait_for(created.is_file)
        click(wid, 548, 96)
        presets = json.loads(call("presets", "--json"))["presets"]
        assert next(p for p in presets if p["name"] == "Editor-preset")["autoapply"]

        before_rename = created.read_bytes()
        call("rename", "Editor-preset", "Portable layout", code=1)
        assert created.read_bytes() == before_rename
        click(wid, 716, 96)
        click(wid, 290, 233)
        run("xdotool", "type", "--delay", "10", "Editor renamed")
        run("xdotool", "key", "Return")
        renamed = config / "presets" / "Editor-renamed.sh"
        wait_for(renamed.is_file)
        assert not created.exists() and renamed.read_bytes() == before_rename
        assert renamed.stat().st_mode & 0o111

        if multi:
            call("layout", "y", "--yes")
        saved = state()["outputs"]
        inode = renamed.stat().st_ino
        click(wid, 716, 96)
        click(wid, 290, 272)
        click(wid, 520, 322)
        wait_for(lambda: renamed.stat().st_ino != inode)
        assert "# autoapply=true" in renamed.read_text()
        if multi:
            assert renamed.read_bytes() != before_rename
            call("preset", "Office dock", "--yes")
            call("preset", "Editor-renamed", "--yes")
            assert state()["outputs"] == saved
        click(wid, 548, 96)

        # The workflow invokes Launch with its picker contract, then runs the chosen action.
        picker = root / "launch"
        picker.write_text("""#!/bin/sh
test "$1" = pick && test "$2" = --require-match || exit 99
choices=$(cat)
printf '%s\\n' "$choices" | grep -Fx "$UNDOCKED_TEST_CHOICE" || exit 127
""")
        picker.chmod(0o755)

        def choose(label):
            os.environ["UNDOCKED_TEST_CHOICE"] = label
            call("launch")

        choose("Identify displays")
        wait_for(lambda: window("Undocked Identify"))
        choose("Switch to Editor-renamed")
        if state()["confirmation_seconds"]:
            choose("Keep display changes")
        if multi:
            choose("Disable DUMMY1")
            assert not state()["outputs"][1]["enabled"]
            choose("Keep display changes")
            choose("Enable DUMMY1")
            assert state()["outputs"][1]["enabled"]
            choose("Revert display changes")
            assert not state()["outputs"][1]["enabled"]
            choose("Switch to Editor-renamed")
            assert state()["outputs"][1]["enabled"]
            choose("Keep display changes")
            choose("Mirror displays")
            assert all(out["position"] == [0, 0] for out in state()["outputs"])
            choose("Keep display changes")
            choose("Restore extended layout")
            assert any(out["position"] != [0, 0] for out in state()["outputs"])
            choose("Keep display changes")
        time.sleep(0.45)
        click(wid, 716, 96)
        click(wid, 290, 311)
        click(wid, 422, 322)
        assert renamed.is_file()
        click(wid, 716, 96)
        click(wid, 290, 311)
        click(wid, 520, 322)
        wait_for(lambda: not renamed.exists())
        assert script.is_file()
        call("overwrite", "Editor-renamed", code=1)
        call("save", "CLI preset", "--auto")
        call("overwrite", "CLI preset")
        call("rename", "CLI preset", "cli-preset")
        assert "# autoapply=true" in (config / "presets" / "cli-preset.sh").read_text()
        call("delete", "cli-preset")
        assert not (config / "presets" / "cli-preset.sh").exists()
        run("xdotool", "windowfocus", wid, "key", "--delay", "150", "Escape", "Escape")
        assert editor.wait(timeout=5) == 0
    finally:
        if editor and editor.poll() is None:
            editor.terminate()
            editor.wait(timeout=5)
        daemon.terminate()
        daemon.wait(timeout=5)

print("X11 checks passed.")
