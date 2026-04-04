"""
Auto-version pre-build script for PlatformIO.

Version scheme:  MAJOR.MINOR.PATCH+BUILD
  major  — manual: bump in version.json for architecture changes
  minor  — auto:   git commit count (monotonically increases with every commit)
  patch  — manual: bump in version.json for named releases / bug-fix milestones
  build  — auto:   incremented every compile (persisted in version.json)

Injects build flags:
  FW_VERSION_MAJOR, FW_VERSION_MINOR, FW_VERSION_PATCH, FW_BUILD_NUMBER,
  FW_VERSION_STR  (e.g. "2.29.0+47")
"""

Import("env")

import json
import os
import subprocess

VERSION_FILE = os.path.join(env.subst("$PROJECT_DIR"), "version.json")


def _git_commit_count():
    try:
        out = subprocess.check_output(
            ["git", "rev-list", "--count", "HEAD"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        )
        return int(out.strip())
    except Exception:
        return 0


def _git_short_hash():
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        )
        return out.strip().decode()
    except Exception:
        return "unknown"


def apply_version(*args, **kwargs):
    with open(VERSION_FILE, "r") as f:
        v = json.load(f)

    major = int(v.get("major", 1))
    patch = int(v.get("patch", 0))
    build = int(v.get("build", 0)) + 1
    minor = _git_commit_count()   # always derived from git
    git_hash = _git_short_hash()

    # Persist incremented build counter
    v["build"] = build
    # Keep minor in the file for reference (read-only, overwritten each time)
    v["_minor_last"] = minor
    v["_git_hash_last"] = git_hash
    with open(VERSION_FILE, "w") as f:
        json.dump(v, f, indent=2)

    version_str = f"{major}.{minor}.{patch}+{build}"
    print(f"[auto_version] {version_str}  (git:{git_hash})")

    env.Append(CPPDEFINES=[
        ("FW_VERSION_MAJOR", str(major)),
        ("FW_VERSION_MINOR", str(minor)),
        ("FW_VERSION_PATCH", str(patch)),
        ("FW_BUILD_NUMBER",  str(build)),
        ("FW_GIT_HASH",      f'\\"{git_hash}\\"'),
        ("FW_VERSION_STR",   f'\\"{version_str}\\"'),
    ])


env.AddPreAction("buildprog", apply_version)
