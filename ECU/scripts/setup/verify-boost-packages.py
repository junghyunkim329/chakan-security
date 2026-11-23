#!/usr/bin/env python3
"""Validate approved Debian inputs and prepare a private, clean Boost sysroot.

Trust boundary: reviewed manifest + host APT keyring; the build user and host
are trusted. Protecting against a compromised build user requires an external
build service. Package maintainer scripts are never executed.
"""
import argparse
import ctypes
import fcntl
import hashlib
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile


def read_manifest(path):
    entries = []
    names, files = set(), set()
    for number, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 6:
            raise ValueError(f"manifest line {number}: expected six fields")
        package, version, arch, filename, size, digest = fields
        if (not re.fullmatch(r"[a-z0-9][a-z0-9+.-]+", package)
                or not re.fullmatch(r"[0-9][A-Za-z0-9.+:~\-]*", version)
                or not re.fullmatch(r"[a-z0-9][a-z0-9-]*", arch)
                or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.+~%\-]*\.deb", filename)
                or not re.fullmatch(r"[1-9][0-9]*", size)
                or not re.fullmatch(r"[0-9a-f]{64}", digest)
                or package in names or filename in files):
            raise ValueError(f"manifest line {number}: invalid or duplicate entry")
        names.add(package)
        files.add(filename)
        entries.append((package, version, arch, filename, int(size), digest))
    if not entries:
        raise ValueError("empty manifest")
    return entries


def verify(entries, directory):
    directory = Path(directory)
    expected = {row[3] for row in entries}
    actual = {p.name for p in directory.iterdir() if p.name.endswith(".deb")}
    if actual != expected:
        raise ValueError(f"package set mismatch: missing={expected - actual}, extra={actual - expected}")
    paths = []
    for package, version, arch, filename, size, digest in entries:
        path = directory / filename
        if not stat.S_ISREG(path.lstat().st_mode) or path.stat().st_size != size:
            raise ValueError(f"{filename}: not a regular file or size mismatch")
        with path.open("rb") as stream:
            actual_digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if actual_digest != digest:
            raise ValueError(f"{filename}: SHA-256 mismatch")
        metadata = subprocess.run(
            ["dpkg-deb", "-f", str(path), "Package", "Version", "Architecture"],
            check=True, capture_output=True, text=True).stdout
        fields = dict(line.split(": ", 1) for line in metadata.splitlines())
        if fields != {"Package": package, "Version": version, "Architecture": arch}:
            raise ValueError(f"{filename}: control metadata mismatch")
        paths.append(path)
    return paths


def prepare(entries, root, packages=None):
    """Download or copy into private staging; verify all before any extraction.

    packages is an offline input directory, subject to the same verification.
    Existing roots are exchanged atomically with Linux renameat2, not merged.
    """
    root = Path(root).absolute()
    root.parent.mkdir(parents=True, exist_ok=True)
    if root.is_symlink() or (root.exists() and not root.is_dir()):
        raise ValueError("Boost root must be a directory, not a symlink")
    with tempfile.TemporaryDirectory(prefix=".boost-stage-", dir=root.parent) as stage:
        stage = Path(stage)
        download = stage / "debs"
        download.mkdir()
        if packages is None:
            subprocess.run(["apt-get", "-o", "APT::Get::AllowUnauthenticated=false",
                            "-o", "Acquire::AllowInsecureRepositories=false", "download",
                            *[f"{p}:{a}={v}" for p, v, a, *_ in entries]],
                           cwd=download, check=True)
        else:
            # Check the whole input set before copying; never follow symlinks.
            for path in verify(entries, packages):
                shutil.copyfile(path, download / path.name)
        paths = verify(entries, download)
        new_root = stage / "root"
        new_root.mkdir()
        for path in paths:
            subprocess.run(["dpkg-deb", "-x", str(path), str(new_root)], check=True)
        if root.exists():
            libc = ctypes.CDLL(None, use_errno=True)
            rename = libc.renameat2
            rename.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_int,
                               ctypes.c_char_p, ctypes.c_uint]
            rename.restype = ctypes.c_int
            if rename(-100, os.fsencode(new_root), -100, os.fsencode(root), 2):
                error = ctypes.get_errno()
                raise OSError(error, os.strerror(error))
        else:
            new_root.rename(root)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[2]
                        / "config/dependencies/boost-debs.manifest")
    parser.add_argument("--packages", type=Path, help="offline directory, verified before use")
    parser.add_argument("--prepare-root", type=Path)
    args = parser.parse_args()
    try:
        entries = read_manifest(args.manifest)
        if args.prepare_root:
            args.prepare_root.parent.mkdir(parents=True, exist_ok=True)
            # Serialize preparation by cooperating callers. The shell holds a
            # separate lock throughout compilation, covering readers as well.
            with (args.prepare_root.parent / ".boost-prepare.lock").open("a") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX)
                prepare(entries, args.prepare_root, args.packages)
        elif args.packages:
            verify(entries, args.packages)
        else:
            parser.error("provide --packages or --prepare-root")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Boost integrity: {error}\n")
    print(f"Boost integrity: {len(entries)} approved packages verified")


if __name__ == "__main__":
    main()
