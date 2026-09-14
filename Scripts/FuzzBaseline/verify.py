# Compare a PIE fuzz trace with its golden file; run inside the editor.
#   import sys; sys.path.insert(0, r"<Project>/Plugins/Claireon/Scripts/FuzzBaseline")
#   import verify; verify.run("Conductor", map_path="/Claireon/FuzzBaseline/L_Fuzz_Baseline")

import claireon
import glob
import os
import re
import sys
import time


def _script_dir():
    """This directory. Uses __file__ when the module was imported normally, and
    otherwise the sys.path entry the caller added in order to import it."""
    if "__file__" in globals():
        return os.path.dirname(os.path.abspath(__file__))
    for entry in sys.path:
        if entry and os.path.isfile(os.path.join(entry, "fuzztrace.py")):
            return os.path.abspath(entry)
    raise RuntimeError(
        "cannot locate the FuzzBaseline script directory; add it to sys.path "
        "before importing verify")


def _log_dir():
    """The project's Saved/Logs directory. Asks the engine first; falls back to
    this script's location (Plugins/Claireon/Scripts/FuzzBaseline, so the project
    root is four levels up)."""
    try:
        import unreal
        return os.path.abspath(unreal.Paths.project_log_dir())
    except Exception:
        return os.path.abspath(os.path.join(
            _script_dir(), os.pardir, os.pardir, os.pardir, os.pardir,
            "Saved", "Logs"))


def _newest_log():
    """The log the running editor is writing. Chosen by filter rather than by
    name so this works in any project: the most recently modified top-level
    *.log in Saved/Logs, ignoring rotated backups."""
    found = [p for p in glob.glob(os.path.join(_log_dir(), "*.log"))
             if "-backup-" not in os.path.basename(p)]
    if not found:
        raise RuntimeError("no *.log found in %s" % _log_dir())
    return max(found, key=os.path.getmtime)


DIR = _script_dir()


def _read_trace(stream_name, since_marker):
    """Return the last complete run of FUZZ lines for stream_name from the log,
    normalized (grouping commas stripped from the DONE checksum)."""
    pat = re.compile(r"%s: (FUZZ\|[^\r\n]+)" % re.escape(stream_name))
    lines = []
    with open(_newest_log(), "r", encoding="utf-8", errors="ignore") as f:
        for ln in f:
            m = pat.search(ln)
            if m:
                lines.append(m.group(1).strip())
    # Read from the last start marker onward.
    starts = [i for i, s in enumerate(lines) if since_marker in s]
    if not starts:
        return []
    run = lines[starts[-1]:]
    return [s.replace(",", "") for s in run]


def run(fixture, map_path, stream="Server", start_marker="|MAIN|1|",
        settle_seconds=16, timeout=40):
    golden_path = os.path.join(DIR, "%s_golden.trace" % fixture.lower())
    if not os.path.exists(golden_path):
        raise RuntimeError("no golden file: %s" % golden_path)
    with open(golden_path, "r", encoding="utf-8") as f:
        golden = [l.strip().replace(",", "") for l in f if l.strip()]

    claireon.map_open_async(mapPath=map_path)
    claireon.pie_wait_for(condition="pieReady", timeoutSeconds=30)
    claireon.pie_wait_for(condition="duration",
                          conditionParams={"seconds": settle_seconds},
                          timeoutSeconds=timeout)
    time.sleep(settle_seconds + 3)  # let the async duration wait resolve
    label = "%s:" % stream if stream else ""
    actual = _read_trace(label.rstrip(":") if stream else "", start_marker) \
        if stream else []
    # stream label handling: log prints "<Stream>: FUZZ|..."
    actual = _read_trace(stream, start_marker)
    claireon.pie_stop_async()

    ok = actual == golden
    print("VERIFY %s: %s" % (fixture, "PASS" if ok else "FAIL"))
    print("  golden lines: %d, actual lines: %d" % (len(golden), len(actual)))
    if not ok:
        gl = {s.split("|")[3] if s.count("|") >= 4 else s: s for s in golden}
        for i in range(max(len(golden), len(actual))):
            g = golden[i] if i < len(golden) else "<missing>"
            a = actual[i] if i < len(actual) else "<missing>"
            if g != a:
                print("  DIFF @%d:\n    golden: %s\n    actual: %s" % (i, g, a))
    return ok
