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


def layout(root):
    status = state()
    outputs = status["outputs"]
    assert [out["port"] for out in outputs] == ["DUMMY0", "DUMMY1", "DUMMY2"], outputs
    call("save", "Office dock", "--auto")
    call("layout", "y", "--dry-run")
    call("layout", "y")
    y = state()
    assert y["confirmation_seconds"] > 0
    assert y["outputs"][0]["position"][1] > 0
    call("keep")
    hook = root / "events"
    wait_for(hook.exists)
    call("layout", "mirror")
    mirrored = state()
    assert all(out["position"] == [0, 0] for out in mirrored["outputs"])
    wait_for(lambda: not state()["confirmation_seconds"], seconds=7)
    assert state()["outputs"] == y["outputs"]
    call("layout", "mirror", "--yes")
    call("layout", "extend", "--yes")
    assert state()["outputs"] == y["outputs"]
    call("output", "DUMMY1", "off")
    assert not state()["outputs"][1]["enabled"]
    call("revert")
    assert state()["outputs"][1]["enabled"]
    time.sleep(0.65)
    before = len(hook.read_text().splitlines())

    run("xrandr", "--output", "DUMMY1", "--off", "--set", "non-desktop", "1")
    wait_for(lambda: len(state()["outputs"]) == 2)
    wait_for(lambda: len(hook.read_text().splitlines()) == before + 1)
    run("xrandr", "--output", "DUMMY1", "--set", "non-desktop", "0")
    wait_for(lambda: state()["status"] == "Automatic preset: Office dock")
    final = state()
    assert [out["position"] for out in final["outputs"]] == [out["position"] for out in outputs]
    wait_for(lambda: len(hook.read_text().splitlines()) == before + 2)
    time.sleep(0.5)
    assert len(hook.read_text().splitlines()) == before + 2
    assert hook.read_text().splitlines()[-2:] == ["undock:", "dock:Office dock"]


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
        "[undocked]\nsettle_ms = 180\nconfirm_seconds = 5\nidentify_seconds = 1\n"
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
            run("xdotool", "mousemove", "--window", wid, "175", "250", "mousedown", "1",
                "sleep", "0.2", "mousemove", "--window", wid, "240", "312",
                "sleep", "0.2", "mouseup", "1")
            time.sleep(0.2)
            click(wid, 837, 615)
            wait_for(lambda: state()["confirmation_seconds"] > 0)
            assert state()["outputs"] != original
            click(wid, 740, 615)
            wait_for(lambda: state()["outputs"] == original)
        click(wid, 738, 615)
        run("xdotool", "windowfocus", wid)
        run("xdotool", "type", "--delay", "10", "Editor preset")
        run("xdotool", "key", "Return")
        wait_for(lambda: (config / "presets" / "Editor preset.sh").is_file())

        # The workflow invokes Launch with its picker contract, then runs the chosen action.
        picker = root / "launch"
        picker.write_text("""#!/bin/sh
test "$1" = pick && test "$2" = --require-match || exit 99
choices=$(cat)
printf '%s\\n' "$choices" | grep -Fx 'Switch to Editor preset' >/dev/null || exit 98
printf '%s\\n' 'Identify displays'
""")
        picker.chmod(0o755)
        os.environ["PATH"] = f"{root}:{os.environ['PATH']}"
        call("launch")
        wait_for(lambda: window("Undocked Identify"))
        run("xdotool", "windowfocus", wid, "key", "Escape")
        assert editor.wait(timeout=5) == 0
    finally:
        if editor and editor.poll() is None:
            editor.terminate()
            editor.wait(timeout=5)
        daemon.terminate()
        daemon.wait(timeout=5)

print("X11 checks passed.")
