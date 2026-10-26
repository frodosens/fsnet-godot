#!/usr/bin/env python3
"""Install a verified fsnet desktop binary SDK, without fetching its sources."""

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import tempfile
import urllib.error
import urllib.request
import zipfile


PLATFORMS = ("linux-x86_64", "windows-x86_64", "macos-x86_64", "macos-arm64")


def digest(path):
    value = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def checksum_from_file(path, name):
    matches = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(r"([0-9a-fA-F]{64})\s+\*?(.+)", line)
        if match and match[2] == name:
            matches.append(match[1].lower())
    if len(matches) != 1:
        raise ValueError(f"SHA256SUMS must contain exactly one checksum for {name}")
    return matches[0]


def download(url, destination):
    request = urllib.request.Request(url, headers={"User-Agent": "fsnet-godot-sdk-installer/0.1"})
    try:
        with urllib.request.urlopen(request, timeout=60) as response, Path(destination).open("wb") as target:
            total = 0
            while block := response.read(1024 * 1024):
                total += len(block)
                if total > 512 * 1024 * 1024:
                    raise ValueError("SDK download exceeds 512 MiB")
                target.write(block)
    except urllib.error.HTTPError as error:
        raise RuntimeError(f"SDK release asset download failed (HTTP {error.code})") from None


def safe_unpack(archive, destination):
    """Reject traversal, symlinks, duplicate names and excessive decompression."""
    destination = Path(destination)
    with zipfile.ZipFile(archive) as package:
        names, size = set(), 0
        for member in package.infolist():
            path = PurePosixPath(member.filename)
            if (not member.filename or "\\" in member.filename or "\0" in member.filename
                    or path.is_absolute() or any(part in ("", ".", "..") or ":" in part for part in path.parts)):
                raise ValueError("unsafe SDK archive path")
            normalized = path.as_posix().rstrip("/").casefold()
            if normalized in names:
                raise ValueError("duplicate SDK archive path")
            names.add(normalized)
            mode = member.external_attr >> 16
            if stat.S_ISLNK(mode) or (stat.S_IFMT(mode) not in (0, stat.S_IFREG, stat.S_IFDIR)):
                raise ValueError("SDK archive links/devices are not allowed")
            size += member.file_size
            if member.file_size > 256 * 1024 * 1024 or size > 1024 * 1024 * 1024 or len(names) > 20000:
                raise ValueError("SDK archive exceeds extraction limits")
        for member in package.infolist():
            target = destination.joinpath(*PurePosixPath(member.filename).parts)
            if member.is_dir():
                target.mkdir(parents=True, exist_ok=True)
                continue
            target.parent.mkdir(parents=True, exist_ok=True)
            with package.open(member) as source, target.open("xb") as output:
                while block := source.read(1024 * 1024):
                    output.write(block)
            os.chmod(target, 0o755 if (member.external_attr >> 16) & 0o111 else 0o644)


def install(archive, expected_sha256, prefix, platform, expected_revision=None, provenance=None, expected_version="0.3.1"):
    archive, prefix = Path(archive), Path(prefix).resolve()
    if digest(archive) != expected_sha256.lower():
        raise ValueError("SDK archive SHA256 mismatch")
    if prefix.exists() and any(prefix.iterdir()):
        raise ValueError("SDK prefix is not empty; select a fresh installation directory")
    prefix.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".fsnet-sdk-", dir=prefix.parent) as folder:
        staging = Path(folder) / "sdk"
        staging.mkdir()
        safe_unpack(archive, staging)
        manifest = json.loads((staging / "manifest.json").read_text(encoding="utf-8"))
        if manifest.get("platform") != platform:
            raise ValueError("SDK manifest platform does not match requested platform")
        if (manifest.get("version") != expected_version or manifest.get("wire_version") != 2
                or manifest.get("lua") is not False or manifest.get("postgres") is not False):
            raise ValueError("expected the pinned Lua-free desktop SDK with wire version 2")
        revision = manifest.get("source_revision", "")
        if not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", revision):
            raise ValueError("SDK manifest has no canonical source revision")
        if expected_revision and revision != expected_revision:
            raise ValueError("SDK source revision does not match the pinned revision")
        for required in ("include/fsnet/client.hpp", "lib/cmake/fsnet/fsnetConfig.cmake", "licenses"):
            if not (staging / required).exists():
                raise ValueError(f"SDK is missing {required}")
        runtimes, fingerprints = manifest.get("runtime_files"), manifest.get("runtime_sha256")
        if not isinstance(runtimes, list) or not runtimes or not isinstance(fingerprints, dict) or set(runtimes) != set(fingerprints):
            raise ValueError("SDK must fingerprint each runtime library")
        for name in runtimes:
            path = PurePosixPath(name)
            if (not name or "\\" in name or path.is_absolute()
                    or any(part in ("", ".", "..") or ":" in part for part in path.parts)):
                raise ValueError("unsafe SDK runtime path")
            runtime = staging.joinpath(*path.parts)
            if not runtime.is_file() or digest(runtime) != fingerprints[name]:
                raise ValueError("SDK runtime checksum mismatch")
        (staging / "sdk-archive.json").write_text(json.dumps({
            "asset": archive.name, "sha256": expected_sha256.lower(),
            "source_revision": revision, **(provenance or {}),
        }, indent=2) + "\n", encoding="utf-8")
        if prefix.exists():
            prefix.rmdir()
        staging.rename(prefix)
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", default="frodosens/fsnet")
    parser.add_argument("--version", default="0.3.1")
    parser.add_argument("--tag", default="sdk-v0.3.1")
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--prefix", type=Path, required=True)
    parser.add_argument("--archive", type=Path, help="existing SDK ZIP; no network when supplied")
    parser.add_argument("--checksums", type=Path, help="SHA256SUMS for an existing SDK ZIP")
    parser.add_argument("--sha256", help="pinned SHA256, instead of a checksum file")
    parser.add_argument("--expected-revision", help="exact SDK source commit")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        parser.error("repository must be owner/name")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", args.version):
        parser.error("invalid SDK version")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", args.tag):
        parser.error("invalid SDK release tag")
    if args.sha256 and not re.fullmatch(r"[0-9a-fA-F]{64}", args.sha256):
        parser.error("invalid SHA256")
    asset = f"fsnet-sdk-v{args.version}-{args.platform}.zip"
    with tempfile.TemporaryDirectory(prefix="fsnet-sdk-download-") as folder:
        temporary = Path(folder)
        archive, checksums = args.archive, args.checksums
        if archive is None:
            archive = temporary / asset
            base = f"https://github.com/{args.repository}/releases/download/{args.tag}"
            download(base + "/" + asset, archive)
            if not args.sha256:
                checksums = temporary / "SHA256SUMS"
                download(base + "/SHA256SUMS", checksums)
        if not args.sha256 and checksums is None:
            parser.error("--archive needs --sha256 or --checksums")
        expected = args.sha256 or checksum_from_file(checksums, asset)
        manifest = install(archive, expected, args.prefix, args.platform, args.expected_revision,
                           {"repository": args.repository, "release_tag": args.tag}, args.version)
    print(json.dumps({"prefix": str(args.prefix.resolve()), "platform": args.platform,
                      "source_revision": manifest["source_revision"], "archive_sha256": expected}))


if __name__ == "__main__":
    main()
