"""Archive safety tests; synthetic binary headers are not native integration proof."""

import importlib.util
import json
import os
from pathlib import Path
import stat
import struct
import sys
import tempfile
import unittest
from unittest import mock
import zipfile


ROOT = Path(__file__).resolve().parents[1]


def module(name):
    if str(ROOT / "tools") not in sys.path:
        sys.path.insert(0, str(ROOT / "tools"))
    spec = importlib.util.spec_from_file_location(name, ROOT / "tools" / (name + ".py"))
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


fetch = module("fetch_sdk")
pack = module("package")
consumer = module("verify_package")


class SDKArchiveTests(unittest.TestCase):
    def test_checksum_entry_is_exact_and_unique(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "SHA256SUMS"
            path.write_text("a" * 64 + "  sdk.zip\n" + "b" * 64 + "  other.zip\n")
            self.assertEqual(fetch.checksum_from_file(path, "sdk.zip"), "a" * 64)
            path.write_text(path.read_text() + "c" * 64 + "  sdk.zip\n")
            with self.assertRaises(ValueError):
                fetch.checksum_from_file(path, "sdk.zip")

    def test_traversal_duplicate_and_symlink_are_rejected_before_extracting(self):
        fixtures = ["../escape", "/absolute", "C:/windows", "folder\\escape", "null\0suffix"]
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            for index, name in enumerate(fixtures):
                archive = root / f"unsafe-{index}.zip"
                with zipfile.ZipFile(archive, "w") as stream:
                    stream.writestr("safe.txt", "safe")
                    # Preserve the actual ZIP member name even on Windows,
                    # where ZipInfo's constructor normalizes backslashes.
                    entry = zipfile.ZipInfo()
                    entry.filename = entry.orig_filename = name
                    stream.writestr(entry, "unsafe")
                target = root / f"out-{index}"
                with self.assertRaises(ValueError):
                    fetch.safe_unpack(archive, target)
                self.assertFalse(target.exists())
            archive = root / "linked.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                entry = zipfile.ZipInfo("include/link")
                entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                stream.writestr(entry, "../../escape")
            with self.assertRaises(ValueError):
                fetch.safe_unpack(archive, root / "linked-out")
            archive = root / "case-duplicate.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("lib/fsnet.dll", "first")
                stream.writestr("LIB/FSNET.DLL", "second")
            with self.assertRaises(ValueError):
                fetch.safe_unpack(archive, root / "duplicate-out")

    def test_installer_checks_hash_platform_revision_and_never_overwrites(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            archive = root / "sdk.zip"
            with zipfile.ZipFile(archive, "w") as stream:
                stream.writestr("manifest.json", json.dumps({"platform": "linux-x86_64", "source_revision": "a" * 40,
                    "version": "0.3.1", "wire_version": 2, "lua": False, "postgres": False,
                    "runtime_files": ["lib/libfsnet.so"], "runtime_sha256": {"lib/libfsnet.so": pack.digest(b"fixture runtime")}}))
                stream.writestr("include/fsnet/client.hpp", "public API")
                stream.writestr("lib/cmake/fsnet/fsnetConfig.cmake", "installed target")
                stream.writestr("licenses/LICENSE.txt", "dependency license")
                stream.writestr("lib/libfsnet.so", "fixture runtime")
            prefix = root / "installed"
            for checksum, platform, revision in [("0" * 64, "linux-x86_64", None),
                                                  (fetch.digest(archive), "windows-x86_64", None),
                                                  (fetch.digest(archive), "linux-x86_64", "b" * 40)]:
                with self.assertRaises(ValueError):
                    fetch.install(archive, checksum, prefix, platform, revision)
                self.assertFalse(prefix.exists())
            fetch.install(archive, fetch.digest(archive), prefix, "linux-x86_64", "a" * 40)
            self.assertTrue((prefix / "include/fsnet/client.hpp").is_file())
            with self.assertRaises(ValueError):
                fetch.install(archive, fetch.digest(archive), prefix, "linux-x86_64")


class PluginPackageTests(unittest.TestCase):
    def test_platform_filter_preserves_only_matching_library_and_dependencies(self):
        source = ('[configuration]\nentry_symbol="fsnet_library_init"\n[libraries]\n'
                  'linux.debug.x86_64="res://addons/fsnet/bin/libfsnet_godot.linux.x86_64.so"\n'
                  'windows.debug.x86_64="res://addons/fsnet/bin/fsnet_godot.windows.x86_64.dll"\n'
                  '[dependencies]\nlinux.x86_64={\n"res://addons/fsnet/bin/libfsnet.so":""\n}\n'
                  'windows.x86_64={"res://addons/fsnet/bin/fsnet.dll":""}\n')
        filtered, references = pack.filter_extension(source, "linux-x86_64")
        self.assertNotIn("windows", filtered)
        self.assertEqual(references, {"libfsnet_godot.linux.x86_64.so", "libfsnet.so"})

    def test_matching_platform_binary_headers_required(self):
        elf = bytearray(64)
        elf[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", elf, 18, 62)
        pack.check_binary(elf, "linux-x86_64")
        for platform in ("windows-x86_64", "macos-arm64", "macos-x86_64"):
            with self.assertRaises(ValueError):
                pack.check_binary(elf, platform)

    def test_unverified_flavor_creates_no_archive(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            report = root / "report.json"
            report.write_text(json.dumps({"status": "passed", "platform": "linux-x86_64", "gdscript": True, "dotnet": False}))
            with self.assertRaises(ValueError):
                pack.package(root, root, root, root, report, root / "out", "0.1.0", "linux-x86_64", ("gdscript", "dotnet"))
            self.assertFalse((root / "out").exists())

    def test_two_flavors_include_only_allowlisted_addon_and_native_files(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            addon, sdk, bindings = root / "addons/fsnet", root / "sdk", root / "bindings"
            (addon / "bin").mkdir(parents=True)
            (addon / "dotnet").mkdir()
            (sdk / "lib").mkdir(parents=True)
            (sdk / "licenses").mkdir()
            bindings.mkdir()
            (root / "docs").mkdir()
            for name in ("README.md", "LICENSE"):
                (root / name).write_text("fixture " + name)
            for name in ("plugin.cfg", "plugin.gd", "fsnet.gd"):
                (addon / name).write_text("fixture " + name)
            (addon / "dotnet/FsnetConnection.cs").write_text("// C# facade fixture")
            (addon / "secret.key").write_text("must not be packaged")
            (addon / "untracked.json").write_text("must not be packaged")
            (sdk / "licenses/OPENSSL.txt").write_text("dependency license fixture")
            (bindings / "LICENSE.md").write_text("Godot bindings license fixture")
            (addon / "fsnet.gdextension").write_text(
                '[libraries]\nlinux.x86_64="res://addons/fsnet/bin/libfsnet_godot.linux.x86_64.so"\n'
                '[dependencies]\nlinux.x86_64={"res://addons/fsnet/bin/libfsnet.so":""}\n')
            binary = bytearray(64)
            binary[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", binary, 18, 62)
            (addon / "bin/libfsnet_godot.linux.x86_64.so").write_bytes(binary)
            (sdk / "lib/libfsnet.so").write_bytes(binary)
            (sdk / "manifest.json").write_text(json.dumps({
                "platform": "linux-x86_64", "source_revision": "a" * 40,
                "runtime_files": ["lib/libfsnet.so"], "runtime_sha256": {"lib/libfsnet.so": pack.digest(binary)},
            }))
            report = root / "report.json"
            report.write_text(json.dumps({"status": "passed", "platform": "linux-x86_64", "gdscript": True, "dotnet": True,
                                          "native_sha256": {"libfsnet_godot.linux.x86_64.so": pack.digest(binary), "libfsnet.so": pack.digest(binary)}}))
            with mock.patch.object(pack.subprocess, "check_output", side_effect=["b" * 40 + "\n", ""]):
                results = pack.package(root, addon, sdk, bindings, report, root / "out", "0.1.0", "linux-x86_64", ("gdscript", "dotnet"))
            self.assertEqual(len(results), 2)
            for result in results:
                with zipfile.ZipFile(root / "out" / result["name"]) as package:
                    names = package.namelist()
                    self.assertTrue(all(name.startswith("addons/fsnet/") for name in names))
                    self.assertFalse(any("secret" in name or "untracked" in name for name in names))
                    self.assertEqual(any(name.endswith(".cs") for name in names), result["flavor"] == "dotnet")
                    self.assertIn("addons/fsnet/licenses/fsnet/OPENSSL.txt", names)
                    manifest = json.loads(package.read("addons/fsnet/package-manifest.json"))
                    self.assertEqual(manifest["sdk_source_revision"], "a" * 40)
                    self.assertFalse(manifest["source_dirty"])
            changed = bytearray(binary)
            changed[-1] = 1
            (addon / "bin/libfsnet_godot.linux.x86_64.so").write_bytes(changed)
            with mock.patch.object(pack.subprocess, "check_output", side_effect=["b" * 40 + "\n", ""]):
                with self.assertRaisesRegex(ValueError, "changed after"):
                    pack.package(root, addon, sdk, bindings, report, root / "tampered-out", "0.1.0", "linux-x86_64", ("gdscript",))
            self.assertFalse((root / "tampered-out").exists())


class ConsumerEnvironmentTests(unittest.TestCase):
    def test_writable_profile_and_explicit_dotnet_are_used_without_sdk_search_paths(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            sdk = root / "selected-dotnet"
            sdk.mkdir()
            executable = sdk / ("dotnet.exe" if os.name == "nt" else "dotnet")
            executable.write_text("fixture")
            before_home = os.environ.get("HOME")
            with mock.patch.dict(os.environ, {"CMAKE_PREFIX_PATH": "must-not-be-used", "LD_LIBRARY_PATH": "must-not-be-used"}):
                env, command = consumer.consumer_environment(root, str(executable), "linux-x86_64")
            self.assertEqual(command, str(executable.resolve()))
            self.assertEqual(env["DOTNET_ROOT"], str(sdk.resolve()))
            self.assertEqual(env["DOTNET_ROOT_X64"], str(sdk.resolve()))
            self.assertEqual(env.get("HOME"), before_home)
            self.assertNotIn("CMAKE_PREFIX_PATH", env)
            self.assertNotIn("LD_LIBRARY_PATH", env)
            for name in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME", "XDG_RUNTIME_DIR",
                         "DOTNET_CLI_HOME", "NUGET_PACKAGES", "NUGET_HTTP_CACHE_PATH"):
                self.assertTrue(Path(env[name]).is_dir(), name)
                self.assertTrue(Path(env[name]).is_relative_to(root), name)


if __name__ == "__main__":
    unittest.main()
