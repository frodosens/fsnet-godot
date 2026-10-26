#!/usr/bin/env python3
"""Collect exactly four validated desktop targets into an eight-package release."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import zipfile

from package import PLATFORMS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--version", default="0.1.0")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", args.version):
        parser.error("invalid semantic release version")
    results, revisions, sdk_revisions = {}, set(), set()
    reports = list(args.artifacts.rglob("package-verification.json"))
    if len(reports) != len(PLATFORMS):
        raise ValueError("expected one packaged-consumer verification report for each desktop target")
    for path in reports:
        if path.is_symlink():
            raise ValueError("verification reports cannot be symlinks")
        verification = json.loads(path.read_text())
        if verification.get("status") != "passed":
            raise ValueError("a packaged-consumer validation failed")
        target_packages = verification.get("packages", [])
        if len(target_packages) != 2 or len({item.get("platform") for item in target_packages}) != 1:
            raise ValueError("each target report must validate exactly its two language packages")
        for result in target_packages:
            key = (result.get("platform"), result.get("flavor"))
            if key in results or key[0] not in PLATFORMS or key[1] not in ("gdscript", "dotnet") or result.get("status") != "passed":
                raise ValueError("duplicate, unvalidated or unsupported release package")
            name = result.get("asset", "")
            expected_name = f"fsnet-godot-v{args.version}-{key[1]}-{key[0]}.zip"
            if name != expected_name:
                raise ValueError("release asset has unexpected name/version")
            archive = path.parent / name
            if archive.is_symlink():
                raise ValueError("release assets cannot be symlinks")
            checksum = hashlib.sha256(archive.read_bytes()).hexdigest()
            if checksum != result.get("sha256"):
                raise ValueError("release asset changed after its packaged-consumer check")
            with zipfile.ZipFile(archive) as package:
                manifest = json.loads(package.read("addons/fsnet/package-manifest.json"))
            if manifest.get("source_dirty") is not False or manifest.get("platform") != key[0] or manifest.get("flavor") != key[1] or manifest.get("version") != args.version:
                raise ValueError("release package provenance does not match verification")
            if manifest.get("source_revision") != result.get("source_revision") or manifest.get("sdk_source_revision") != result.get("sdk_source_revision"):
                raise ValueError("package source provenance differs from its packaged-consumer report")
            integration = manifest.get("validation", {})
            if integration.get("status") != "passed" or integration.get("platform") != key[0] or integration.get(key[1]) is not True:
                raise ValueError("package has no matching successful network integration")
            validated_native = integration.get("native_sha256", {})
            native_files = {name.removeprefix("bin/"): checksum for name, checksum in manifest.get("files_sha256", {}).items() if name.startswith("bin/")}
            if not native_files or native_files != validated_native:
                raise ValueError("packaged native binaries differ from the network validation fingerprints")
            revisions.add(manifest["source_revision"])
            sdk_revisions.add(manifest["sdk_source_revision"])
            results[key] = {"name": name, "sha256": checksum, "size": archive.stat().st_size,
                            "platform": key[0], "flavor": key[1], "source": archive}
    if set(results) != {(platform, flavor) for platform in PLATFORMS for flavor in ("gdscript", "dotnet")}:
        raise ValueError("all four desktop targets must pass both GDScript and .NET packaged-consumer validation")
    if len(revisions) != 1 or len(sdk_revisions) != 1:
        raise ValueError("all release packages must use the same plugin and SDK source revisions")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    packages = []
    for key in sorted(results):
        item = dict(results[key])
        source = item.pop("source")
        shutil.copy2(source, args.output_dir / item["name"])
        packages.append(item)
    (args.output_dir / "SHA256SUMS").write_text("".join(f"{item['sha256']}  {item['name']}\n" for item in packages))
    (args.output_dir / "release-manifest.json").write_text(json.dumps({
        "version": args.version, "source_revision": next(iter(revisions)),
        "sdk_source_revision": next(iter(sdk_revisions)), "packages": packages,
    }, indent=2) + "\n")
    print(json.dumps({"packages": len(packages), "source_revision": next(iter(revisions)),
                      "output_dir": str(args.output_dir.resolve())}))


if __name__ == "__main__":
    main()
