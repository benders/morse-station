#!/usr/bin/env python3
"""PlatformIO pre-build hook: inject the current git revision as -DGIT_REV.

Adds a build flag like GIT_REV=\"a1b2c3d\" (or \"a1b2c3d-dirty\") so the
firmware can print which build it is on boot.
"""
import subprocess

Import("env")  # noqa: F821  (provided by PlatformIO's SCons environment)


def git_rev() -> str:
    try:
        desc = subprocess.check_output(
            ["git", "describe", "--always", "--dirty", "--tags"],
            stderr=subprocess.DEVNULL,
        )
        return desc.decode().strip()
    except Exception:
        return "unknown"


rev = git_rev()
env.Append(CPPDEFINES=[("GIT_REV", env.StringifyMacro(rev))])
print(f"git_rev.py: GIT_REV={rev}")
