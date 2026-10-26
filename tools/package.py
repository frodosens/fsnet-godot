#!/usr/bin/env python3
"""Package only validated native Godot addons and their binary SDK dependencies."""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import struct
import subprocess
import zipfile


ROOT = Path(__file__).resolve().parents[1]
PLATFORMS = ("linux-x86_64", "windows-x86_64", "macos-x86_64", "macos-arm64")
MODULES = {
    "linux-x86_64": "libfsnet_godot.linux.x86_64.so",
    "windows-x86_64": "fsnet_godot.windows.x86_64.dll",
    "macos-x86_64": "libfsnet_godot.macos.x86_64.dylib",
    "macos-arm64": "libfsnet_godot.macos.arm64.dylib",
}
RUNTIMES = {"linux": "libfsnet.so", "windows": "fsnet.dll", "macos": "libfsnet.dylib"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def safe_relative(value):
    path = PurePosixPath(value)
    if (not value or "\\" in value or "\0" in value or path.is_absolute()
            or any(part in ("", ".", "..") or ":" in part for part in path.parts)):
        raise ValueError("unsafe SDK runtime path")
    return path


def sdk_runtime_paths(manifest):
    values = manifest.get("runtime_files", [])
    if isinstance(values, dict):
        values = list(values)
    result = []
    for value in values:
        if isinstance(value, dict):
            value = value.get("path", "")
        path = safe_relative(value)
        if not re.search(r"(?:\.dll|\.dylib|\.so(?:\.[0-9]+)*)$", path.name, re.I):
            raise ValueError("SDK runtime_files must list shared libraries only")
        result.append(path)
    return result


def check_binary(data, platform):
    if platform == "linux-x86_64":
        if len(data) < 24 or data[:5] != b"\x7fELF\x02" or data[5] != 1 or struct.unpack_from("<H", data, 18)[0] != 62:
            raise ValueError("expected Linux x86_64 ELF library")
    elif platform == "windows-x86_64":
        if len(data) < 64 or data[:2] != b"MZ":
            raise ValueError("expected Windows x86_64 PE library")
        offset = struct.unpack_from("<I", data, 60)[0]
        if offset + 6 > len(data) or data[offset:offset + 4] != b"PE\0\0" or struct.unpack_from("<H", data, offset + 4)[0] != 0x8664:
            raise ValueError("expected Windows x86_64 PE library")
    else:
        wanted = 0x0100000C if platform.endswith("arm64") else 0x01000007
        if len(data) < 8:
            raise ValueError("expected macOS Mach-O library")
        if data[:4] == b"\xcf\xfa\xed\xfe":
            architectures = [struct.unpack_from("<I", data, 4)[0]]
        elif data[:4] in (b"\xca\xfe\xba\xbe", b"\xca\xfe\xba\xbf"):
            count = struct.unpack_from(">I", data, 4)[0]
            stride = 32 if data[:4] == b"\xca\xfe\xba\xbf" else 20
            if count > 16 or len(data) < 8 + count * stride:
                raise ValueError("invalid universal Mach-O header")
            architectures = [struct.unpack_from(">I", data, 8 + index * stride)[0] for index in range(count)]
        else:
            raise ValueError("expected macOS Mach-O library")
        if wanted not in architectures:
            raise ValueError("Mach-O library does not contain requested architecture")


def filter_extension(source, platform):
    os_name, arch = platform.split("-", 1)
    section, keep, level = "", True, 0
    lines = []
    for line in source.splitlines():
        match = re.fullmatch(r"\s*\[([^]]+)\]\s*", line)
        if match:
            section, keep, level = match[1], True, 0
            lines.append(line)
            continue
        entry = re.match(r"\s*([A-Za-z0-9_.]+)\s*=", line)
        if section in ("libraries", "dependencies") and entry and level == 0:
            tags = entry[1].split(".")
            keep = os_name in tags and arch in tags
        if keep:
            lines.append(line)
        if section in ("libraries", "dependencies"):
            # Godot dependency dictionaries may be formatted on multiple lines.
            quoted = re.sub(r'"(?:[^"\\]|\\.)*"', '""', line)
            level += quoted.count("{") - quoted.count("}")
            if level < 0:
                raise ValueError("invalid GDExtension dependency dictionary")
    if level:
        raise ValueError("unterminated GDExtension dependency dictionary")
    filtered = "\n".join(lines) + "\n"
    references = set(re.findall(r'"res://addons/fsnet/bin/([^"/]+)"', filtered))
    if MODULES[platform] not in references:
        raise ValueError("GDExtension manifest has no matching platform library")
    return filtered, references


def json_bytes(value):
    return (json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n").encode()


def package(root, addon, sdk, bindings, validation_path, output, version, platform, flavors):
    validation = json.loads(validation_path.read_text(encoding="utf-8"))
    if validation.get("status") != "passed" or validation.get("platform") != platform:
        raise ValueError("a passing validation report for this exact platform is required")
    for flavor in flavors:
        if validation.get(flavor) is not True:
            raise ValueError(f"validation report does not record a passing {flavor} integration")
    sdk_manifest = json.loads((sdk / "manifest.json").read_text(encoding="utf-8"))
    if sdk_manifest.get("platform") != platform or not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", sdk_manifest.get("source_revision", "")):
        raise ValueError("SDK manifest must match platform and identify its source revision")
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain", "--untracked-files=all"], cwd=root, text=True).strip())
    if dirty:
        raise ValueError("commit reviewed sources before producing release packages")
    if validation.get("source_revision") not in (None, revision):
        raise ValueError("validation report belongs to another plugin source revision")
    if validation.get("sdk_source_revision") not in (None, sdk_manifest["source_revision"]):
        raise ValueError("validation report belongs to another SDK source revision")
    extension, references = filter_extension((addon / "fsnet.gdextension").read_text(encoding="utf-8"), platform)
    files = {"fsnet.gdextension": extension.encode()}
    for name in ("plugin.cfg", "plugin.gd", "fsnet.gd"):
        files[name] = (addon / name).read_bytes()
    for name in ("README.md", "LICENSE"):
        files[name] = (root / name).read_bytes()
    for source in sorted((addon).glob("*.svg")):
        files[source.name] = source.read_bytes()
    runtime_paths = sdk_runtime_paths(sdk_manifest)
    expected_runtime = RUNTIMES[platform.split("-", 1)[0]]
    if expected_runtime not in {path.name for path in runtime_paths}:
        raise ValueError("SDK runtime manifest is missing its core shared library")
    native = {MODULES[platform]: addon / "bin" / MODULES[platform]}
    for path in runtime_paths:
        if path.name in native:
            raise ValueError("duplicate runtime basename")
        native[path.name] = sdk.joinpath(*path.parts)
        if sdk_manifest.get("runtime_sha256", {}).get(path.as_posix()) != digest(native[path.name].read_bytes()):
            raise ValueError("installed SDK runtime no longer matches its vendor manifest")
    if references != set(native):
        raise ValueError("GDExtension dependencies must name exactly the bundled native libraries: "
                         f"missing={sorted(set(native)-references)}, unexpected={sorted(references-set(native))}")
    validated_native = validation.get("native_sha256")
    if not isinstance(validated_native, dict) or set(validated_native) != set(native):
        raise ValueError("validation report must identify every bundled native binary by basename and SHA256")
    for name, source in native.items():
        if source.is_symlink():
            raise ValueError("runtime symlinks must be resolved by SDK packaging")
        data = source.read_bytes()
        check_binary(data, platform)
        if digest(data) != validated_native[name]:
            raise ValueError("native binary changed after successful integration validation: " + name)
        files["bin/" + name] = data
    licenses = sdk / "licenses"
    if not licenses.is_dir():
        raise ValueError("SDK dependency license texts are required")
    for source in sorted(licenses.rglob("*")):
        if not source.is_file():
            continue
        if source.is_symlink() or source.suffix.lower() in (".pem", ".key", ".pfx", ".p12"):
            raise ValueError("unexpected link or credential in SDK licenses")
        files["licenses/fsnet/" + source.relative_to(licenses).as_posix()] = source.read_bytes()
    if not any(name.startswith("licenses/fsnet/") for name in files):
        raise ValueError("SDK dependency license texts are empty")
    files["licenses/godot-cpp/LICENSE"] = (bindings / "LICENSE.md").read_bytes() if (bindings / "LICENSE.md").is_file() else (bindings / "LICENSE").read_bytes()
    files["sdk-manifest.json"] = json_bytes(sdk_manifest)
    files["validation.json"] = json_bytes(validation)
    notices = ("# Packaged third-party components\n\n"
               "The adapter is licensed by LICENSE. Godot-cpp is MIT licensed; its license is under licenses/godot-cpp.\n"
               "The fsnet runtime and its exact source revision are identified by sdk-manifest.json.\n"
               "Runtime dependency license texts and source notices are under licenses/fsnet.\n\n"
               "Upstream source locations (no implementation source is needed to build this addon):\n\n"
               "- fsnet: https://github.com/frodosens/fsnet\n"
               "- Godot C++ bindings: https://github.com/godotengine/godot-cpp (MIT)\n"
               "- Asio: https://github.com/chriskohlhoff/asio (Boost Software License 1.0)\n"
               "- SQLiteCpp: https://github.com/SRombauts/SQLiteCpp (MIT)\n"
               "- SQLite: https://sqlite.org (public domain)\n"
               "- OpenSSL: https://github.com/openssl/openssl (Apache 2.0)\n"
               "- libjuice: https://github.com/paullouisageneau/libjuice (MPL 2.0; unmodified covered source is publicly available there)\n"
               "- MinGW GCC runtime, when included: https://gcc.gnu.org/onlinedocs/libstdc++/manual/license.html (GCC Runtime Library Exception)\n")
    files["THIRD_PARTY_NOTICES.md"] = notices.encode()
    for doc in sorted((root / "docs").glob("*.md")):
        files["docs/" + doc.name] = doc.read_bytes()
    output.mkdir(parents=True, exist_ok=True)
    results = []
    dotnet_sources = sorted((addon / "dotnet").rglob("*.cs")) if "dotnet" in flavors else []
    if "dotnet" in flavors and not dotnet_sources:
        raise ValueError(".NET package has no C# facade")
    for flavor in flavors:
        content = dict(files)
        if flavor == "dotnet":
            for source in dotnet_sources:
                if "obj" in source.parts or "bin" in source.parts or source.is_symlink():
                    raise ValueError("unexpected generated or linked C# source")
                content[source.relative_to(addon).as_posix()] = source.read_bytes()
        manifest = {
            "name": "fsnet-godot", "version": version, "flavor": flavor,
            "platform": platform, "source_revision": revision, "source_dirty": dirty,
            "sdk_source_revision": sdk_manifest["source_revision"],
            "godot_compatibility_minimum": "4.6", "validation": validation,
            "files_sha256": {name: digest(data) for name, data in sorted(content.items())},
        }
        content["package-manifest.json"] = json_bytes(manifest)
        archive = output / f"fsnet-godot-v{version}-{flavor}-{platform}.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as package_zip:
            for name, data in sorted(content.items()):
                if any(part in ("", ".", "..") for part in PurePosixPath(name).parts):
                    raise ValueError("unsafe addon package path")
                entry = zipfile.ZipInfo("addons/fsnet/" + name, date_time=(2026, 1, 1, 0, 0, 0))
                entry.compress_type = zipfile.ZIP_DEFLATED
                entry.external_attr = (0o100755 if name.startswith("bin/") else 0o100644) << 16
                package_zip.writestr(entry, data)
        results.append({"name": archive.name, "sha256": digest(archive.read_bytes()),
                        "size": archive.stat().st_size, "platform": platform, "flavor": flavor})
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--addon-dir", type=Path, default=ROOT / "addons/fsnet")
    parser.add_argument("--sdk-prefix", required=True, type=Path)
    parser.add_argument("--bindings-dir", required=True, type=Path)
    parser.add_argument("--validation", required=True, type=Path)
    parser.add_argument("--platform", required=True, choices=PLATFORMS)
    parser.add_argument("--version", default="0.1.0")
    parser.add_argument("--flavor", choices=("all", "gdscript", "dotnet"), default="all")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "dist")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+(?:[-.][A-Za-z0-9]+)*", args.version):
        parser.error("invalid plugin version")
    flavors = ("gdscript", "dotnet") if args.flavor == "all" else (args.flavor,)
    packages = package(ROOT, args.addon_dir, args.sdk_prefix, args.bindings_dir,
                       args.validation, args.output_dir, args.version, args.platform, flavors)
    previous = args.output_dir / "release-manifest.json"
    combined = {}
    if previous.exists():
        saved = json.loads(previous.read_text(encoding="utf-8"))
        if saved.get("version") != args.version:
            raise ValueError("output directory contains another release version")
        combined.update((item["name"], item) for item in saved.get("packages", []))
    combined.update((item["name"], item) for item in packages)
    all_packages = [combined[name] for name in sorted(combined)]
    for item in all_packages:
        if Path(item["name"]).name != item["name"] or digest((args.output_dir / item["name"]).read_bytes()) != item["sha256"]:
            raise ValueError("existing release asset no longer matches its manifest")
    (args.output_dir / "SHA256SUMS").write_text("".join(f"{item['sha256']}  {item['name']}\n" for item in all_packages))
    (args.output_dir / "release-manifest.json").write_bytes(json_bytes({"version": args.version, "packages": all_packages}))
    print(json.dumps({"packages": packages}, indent=2))


if __name__ == "__main__":
    main()
