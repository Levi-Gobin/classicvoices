"""Writes buildinfo.json, the commit a build comes from, that commit's time and its channel (dev when built from
the dev branch: such a build follows the dev channel once installed), for the dev update channel
(nvda/addon/synthDrivers/_classicvoices/update.py; Android's BuildInfo, the Mac app's Info.plist read the same).

    python3 tools/buildinfo.py [out.json]    (default: the shared _classicvoices folder)
    python3 tools/buildinfo.py --print       commit and time, space-separated
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT = os.path.join(ROOT, "nvda", "addon", "synthDrivers", "_classicvoices", "buildinfo.json")


def info():
    try:
        commit = subprocess.check_output(["git", "-C", ROOT, "rev-parse", "HEAD"], text=True).strip()
        when = int(subprocess.check_output(["git", "-C", ROOT, "log", "-1", "--format=%ct"], text=True).strip())
    except (OSError, subprocess.CalledProcessError, ValueError):
        return {"commit": "", "time": 0, "channel": "stable"}
    # the branch: GitHub Actions names it (its checkout may be detached), else git
    branch = os.environ.get("GITHUB_REF_NAME") or ""
    if not branch:
        try:
            branch = subprocess.check_output(["git", "-C", ROOT, "branch", "--show-current"], text=True).strip()
        except (OSError, subprocess.CalledProcessError):
            branch = ""
    return {"commit": commit, "time": when, "channel": "dev" if branch == "dev" else "stable"}


if __name__ == "__main__":
    i = info()
    if sys.argv[1:] == ["--print"]:
        print(i["commit"], i["time"], i["channel"])
    else:
        out = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
        with open(out, "w", encoding="utf-8") as f:
            json.dump(i, f)
