#!/usr/bin/env python3
"""Download checksum-verified official standard and .NET Godot desktop editors."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import urllib.request
import zipfile


PLATFORMS = ("linux-x86_64", "windows-x86_64", "macos-x86_64", "macos-arm64")


def download(url, target):
    request = urllib.request.Request(url, headers={"User-Agent": "fsnet-godot-test-tools/0.1"})
    with urllib.request.urlopen(request, timeout=120) as response, target.open("wb") as output:
        total = 0
        while block := response.read(1024 * 1024):
            total += len(block)
            if total > 512 * 1024 * 1024:
                raise ValueError("Godot download exceeds 512 MiB")
            output.write(block)


def sha512(path):
    value = hashlib.sha512()
    with path.open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def verified_unpack(archive, destination, checksums):
    candidates = []
    for line in checksums.splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{128})\s+\*?(.+)", line)
        if match and match[2] == archive.name:
            candidates.append(match[1].lower())
    if len(candidates) != 1 or sha512(archive) != candidates[0]:
        raise ValueError("Godot official SHA512 checksum mismatch or missing asset checksum")
    with zipfile.ZipFile(archive) as package:
        names, total = set(), 0
        links = []
        for member in package.infolist():
            name = member.orig_filename
            path = PurePosixPath(name)
            if (not name or path.is_absolute() or "\\" in name or "\0" in name
                    or any(part in ("", ".", "..") or ":" in part for part in path.parts)):
                raise ValueError("unsafe Godot archive path")
            normalized = path.as_posix().rstrip("/").casefold()
            if normalized in names:
                raise ValueError("duplicate Godot archive member")
            names.add(normalized)
            total += member.file_size
            if member.file_size > 512 * 1024 * 1024 or total > 2 * 1024 * 1024 * 1024 or len(names) > 50000:
                raise ValueError("Godot archive exceeds extraction limits")
        for member in package.infolist():
            target = destination.joinpath(*PurePosixPath(member.filename).parts)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode):
                links.append((target, package.read(member).decode("utf-8")))
                continue
            if stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR):
                raise ValueError("unsupported Godot archive member type")
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(member) as source, target.open("xb") as output:
                while block := source.read(1024 * 1024):
                    output.write(block)
            os.chmod(target, 0o755 if mode & 0o111 else 0o644)
        # Some official macOS bundles use internal framework symlinks. Create
        # those only after regular files, never follow a link while extracting.
        for target, link in links:
            if "\0" in link or Path(link).is_absolute():
                raise ValueError("unsafe Godot bundle symlink")
            resolved = (target.parent / link).resolve()
            if not resolved.is_relative_to(destination.resolve()):
                raise ValueError("Godot bundle symlink escapes extraction directory")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.symlink_to(link)
        for target, _ in links:
            if not target.resolve().is_relative_to(destination.resolve()):
                raise ValueError("Godot bundle symlink chain escapes extraction directory")
    return candidates[0]


def executable(root, platform, version, mono):
    os_name = platform.split("-", 1)[0]
    if os_name == "macos":
        name = "Godot_mono.app" if mono else "Godot.app"
        candidates = list(root.rglob(name + "/Contents/MacOS/Godot"))
    elif os_name == "linux":
        name = f"Godot_v{version}-stable_" + ("mono_linux.x86_64" if mono else "linux.x86_64")
        candidates = list(root.rglob(name))
    else:
        name = f"Godot_v{version}-stable_" + ("mono_win64" if mono else "win64")
        candidates = list(root.rglob(name + "_console.exe")) or list(root.rglob(name + ".exe"))
    if len(candidates) != 1 or not candidates[0].is_file():
        raise ValueError("official Godot package has no unique expected editor executable")
    os.chmod(candidates[0], 0o755)
    return candidates[0].resolve()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--version", default="4.6.3")
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--output", type=Path, help="tool paths JSON (default prefix/tools.json)")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("invalid Godot version")
    prefix = args.prefix.resolve()
    if prefix.exists() and any(prefix.iterdir()):
        parser.error("Godot tools prefix is not empty; choose a fresh directory")
    prefix.mkdir(parents=True, exist_ok=True)
    base = f"https://github.com/godotengine/godot/releases/download/{args.version}-stable"
    os_name = args.platform.split("-", 1)[0]
    endings = {
        "linux": ("linux.x86_64.zip", "mono_linux_x86_64.zip"),
        "windows": ("win64.exe.zip", "mono_win64.zip"),
        "macos": ("macos.universal.zip", "mono_macos.universal.zip"),
    }[os_name]
    tools = {"version": args.version, "platform": args.platform, "archives_sha512": {}}
    with tempfile.TemporaryDirectory(prefix="fsnet-godot-tools-") as folder:
        temporary = Path(folder)
        checksum_file = temporary / "SHA512-SUMS.txt"
        download(base + "/SHA512-SUMS.txt", checksum_file)
        checksums = checksum_file.read_text(encoding="utf-8")
        for index, ending in enumerate(endings):
            asset = f"Godot_v{args.version}-stable_{ending}"
            archive = temporary / asset
            download(base + "/" + asset, archive)
            target = prefix / ("mono" if index else "standard")
            target.mkdir()
            tools["archives_sha512"][asset] = verified_unpack(archive, target, checksums)
            tools["mono_godot" if index else "godot"] = str(executable(target, args.platform, args.version, bool(index)))
    nupkgs = list((prefix / "mono").rglob("GodotSharp/Tools/nupkgs"))
    if len(nupkgs) != 1:
        raise ValueError("Godot .NET package has no unique offline Godot NuGet feed")
    tools["godot_nuget_source"] = str(nupkgs[0].resolve())
    output = args.output or prefix / "tools.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(tools, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(tools, indent=2))


if __name__ == "__main__":
    main()
