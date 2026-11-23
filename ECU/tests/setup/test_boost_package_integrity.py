"""Exercise the package trust boundary with real .deb fixtures."""
import hashlib
import importlib.util
import subprocess
from pathlib import Path

import pytest

TOOL = Path(__file__).resolve().parents[2] / "scripts/setup/verify-boost-packages.py"


@pytest.fixture
def tool():
    assert TOOL.exists(), "package integrity enforcement is missing"
    spec = importlib.util.spec_from_file_location("boost_integrity", TOOL)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


@pytest.fixture
def package(tmp_path):
    source = tmp_path / "source"
    (source / "DEBIAN").mkdir(parents=True)
    (source / "DEBIAN/control").write_text(
        "Package: sil-fixture\nVersion: 1.0\nArchitecture: amd64\n"
        "Maintainer: SIL <sil@example.invalid>\nDescription: test fixture\n")
    (source / "payload").write_text("approved payload")
    packages = tmp_path / "packages"
    packages.mkdir()
    deb = packages / "sil-fixture_1.0_amd64.deb"
    subprocess.run(["dpkg-deb", "--build", str(source), str(deb)], check=True,
                   capture_output=True)
    manifest = tmp_path / "manifest"
    manifest.write_text("\t".join(["sil-fixture", "1.0", "amd64", deb.name,
                                  str(deb.stat().st_size),
                                  hashlib.sha256(deb.read_bytes()).hexdigest()]) + "\n")
    return manifest, packages, deb


def test_valid_package(tool, package):
    manifest, packages, deb = package
    assert tool.verify(tool.read_manifest(manifest), packages) == [deb]


@pytest.mark.parametrize("change", ["digest", "size", "extra", "missing", "symlink"])
def test_rejects_unapproved_bytes_and_files(tool, package, change):
    manifest, packages, deb = package
    if change == "digest":
        data = bytearray(deb.read_bytes())
        data[-1] ^= 1
        deb.write_bytes(data)
    elif change == "size":
        deb.write_bytes(deb.read_bytes() + b"extra")
    elif change == "extra":
        (packages / "stale.deb").write_bytes(deb.read_bytes())
    elif change == "missing":
        deb.unlink()
    else:
        target = packages.parent / "target.deb"
        deb.rename(target)
        deb.symlink_to(target)
    with pytest.raises(ValueError):
        tool.verify(tool.read_manifest(manifest), packages)


@pytest.mark.parametrize("column,value", [(0, "other-package"), (1, "2.0"), (2, "arm64")])
def test_rejects_metadata_mismatch(tool, package, column, value):
    manifest, packages, _ = package
    fields = manifest.read_text().strip().split("\t")
    fields[column] = value
    manifest.write_text("\t".join(fields) + "\n")
    with pytest.raises(ValueError):
        tool.verify(tool.read_manifest(manifest), packages)


@pytest.mark.parametrize("change", ["duplicate", "path", "empty", "malformed"])
def test_rejects_invalid_manifest(tool, package, change):
    manifest, _, _ = package
    row = manifest.read_text()
    if change == "duplicate":
        manifest.write_text(row + row)
    elif change == "path":
        fields = row.strip().split("\t")
        fields[3] = "../escape.deb"
        manifest.write_text("\t".join(fields))
    else:
        manifest.write_text("" if change == "empty" else "bad row")
    with pytest.raises(ValueError):
        tool.read_manifest(manifest)


def test_prepare_replaces_existing_root_from_verified_packages(tool, package, tmp_path):
    manifest, packages, _ = package
    root = tmp_path / "boost-root"
    root.mkdir()
    (root / "stale").write_text("untrusted")
    tool.prepare(tool.read_manifest(manifest), root, packages)
    assert (root / "payload").read_text() == "approved payload"
    assert not (root / "stale").exists()


def test_failed_prepare_preserves_root(tool, package, tmp_path):
    manifest, packages, deb = package
    root = tmp_path / "boost-root"
    root.mkdir()
    (root / "existing").write_text("preserve")
    deb.write_bytes(b"corrupt")
    with pytest.raises(ValueError):
        tool.prepare(tool.read_manifest(manifest), root, packages)
    assert (root / "existing").read_text() == "preserve"


def test_cli_extracts_and_reports_success(package, tmp_path):
    manifest, packages, _ = package
    root = tmp_path / "cli-root"
    result = subprocess.run(["python3", str(TOOL), "--manifest", str(manifest),
                             "--packages", str(packages), "--prepare-root", str(root)],
                            capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert (root / "payload").read_text() == "approved payload"
    assert "1 approved packages verified" in result.stdout


def test_cli_rejects_extra_package_before_extracting(package, tmp_path):
    manifest, packages, deb = package
    (packages / "stale.deb").write_bytes(deb.read_bytes())
    root = tmp_path / "cli-root"
    result = subprocess.run(["python3", str(TOOL), "--manifest", str(manifest),
                             "--packages", str(packages), "--prepare-root", str(root)],
                            capture_output=True, text=True)
    assert result.returncode == 1
    assert "package set mismatch" in result.stderr
    assert not root.exists()
