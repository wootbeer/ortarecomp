"""Builds a release and zips it for players.

    python tools/package.py

Needs gen/ (tools/recomp.py) first. Builds into build-release/ with no
console window on Windows and the C runtime linked in, then writes
dist/OrtaRecomp-v<version>-windows-x64.zip (Windows; run it from a Visual
Studio developer shell, like a normal build) or
dist/OrtaRecomp-v<version>-linux-x86_64.tar.gz (Linux and Steam Deck: a
tarball keeps the executable bit, and the binary is stripped; the
unstripped one stays in build-release/ to read crash reports), with the
README, CHANGELOG and the licenses.
"""
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import zipfile

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD = os.path.join(ROOT, "build-release")
DIST = os.path.join(ROOT, "dist")


def run(*cmd):
    print("+", " ".join(cmd), flush=True)
    if subprocess.run(cmd, cwd=ROOT).returncode != 0:
        sys.exit(f"error: {cmd[0]} failed")


def main():
    if not os.path.exists(os.path.join(ROOT, "gen", "recomp_dispatch.c")):
        sys.exit("error: no generated code; run tools/recomp.py on your default.xbe first")
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        version = re.search(r"project\(OrtaRecomp VERSION ([0-9.]+)", f.read()).group(1)

    # RelWithDebInfo: the configuration every test so far ran; the symbols
    # stay in build-release/ (the .pdb), for crash reports.
    run("cmake", "-S", ".", "-B", BUILD, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=RelWithDebInfo", "-DPDO_RELEASE=ON",
        # pd1 (gen-pd1/) stays out of releases until it plays.
        "-DPDO_WITH_PD1=OFF")
    run("cmake", "--build", BUILD)

    windows = platform.system() == "Windows"
    exe = "OrtaRecomp.exe" if windows else "OrtaRecomp"
    files = [(os.path.join(BUILD, exe), exe)]
    if not windows:                     # symbols left in build-release/
        stripped = os.path.join(BUILD, "OrtaRecomp.stripped")
        shutil.copyfile(files[0][0], stripped)
        run("strip", "--strip-debug", stripped)
        os.chmod(stripped, 0o755)
        files = [(stripped, exe)]
    files += [(os.path.join(ROOT, n), n) for n in ("README.md", "CHANGELOG.md", "COPYING")]
    files += [(os.path.join(ROOT, "licenses", n), f"licenses/{n}")
              for n in sorted(os.listdir(os.path.join(ROOT, "licenses")))]

    os.makedirs(DIST, exist_ok=True)
    name = f"OrtaRecomp-v{version}-" + ("windows-x64" if windows else f"linux-{platform.machine().lower()}")
    if windows:
        out = os.path.join(DIST, name + ".zip")
        with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as z:
            for src, arc in files:
                z.write(src, arc)
    else:
        out = os.path.join(DIST, name + ".tar.gz")
        with tarfile.open(out, "w:gz") as t:
            for src, arc in files:
                t.add(src, f"{name}/{arc}")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
