#!/usr/bin/env python3
"""Load release ZIPs in fresh Godot projects without an SDK/source checkout."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import xml.sax.saxutils
import zipfile

from fetch_sdk import safe_unpack


GDSCRIPT = '''extends SceneTree
func _initialize():
    assert(ClassDB.class_exists("FsnetClient"), "packaged native extension not loaded")
    assert(ClassDB.class_exists("FsnetMap"), "packaged map class not loaded")
    var client = ClassDB.instantiate("FsnetClient")
    root.add_child(client)
    client.set_auto_poll(false)
    assert(client.get_connection_state() == 0)
    var replies = []
    client.request_completed.connect(func(ticket, status, data): replies.append([ticket, status, data]))
    var ticket = client.call_entity("000000000000002a", "noop", PackedByteArray())
    assert(replies.is_empty(), "request callback fired synchronously")
    client.poll()
    assert(replies.size() == 1 and replies[0][0] == ticket and replies[0][1] != 0)
    client.free()
    print("PASS packaged GDScript addon")
    quit(0)
'''

CSHARP = '''using System;
using System.Threading;
using Godot;
using Fsnet.Godot;
public partial class PackagedSmoke : Node
{
    public override async void _Ready()
    {
        try
        {
            if (!ClassDB.ClassExists("FsnetClient")) throw new Exception("native extension not loaded");
            var connection = new FsnetConnection();
            AddChild(connection);
            if (connection.ConnectionState != FsnetConnectionState.Disconnected) throw new Exception("invalid initial state");
            using var deadline = new CancellationTokenSource(TimeSpan.FromSeconds(3));
            var response = await connection.RpcAsync("000000000000002a", "noop", cancellationToken: deadline.Token);
            if (response.IsSuccess) throw new Exception("disconnected request unexpectedly succeeded");
            connection.CloseConnection();
            GD.Print("PASS packaged .NET addon");
            GetTree().Quit(0);
        }
        catch (Exception error) { GD.PushError(error.ToString()); GetTree().Quit(1); }
    }
}
'''


def run(command, folder, env, timeout=60):
    result = subprocess.run(command, cwd=folder, env=env, capture_output=True, text=True, timeout=timeout)
    if result.returncode != 0 or "SCRIPT ERROR:" in result.stdout + result.stderr:
        raise RuntimeError("packaged addon check failed:\n" + result.stdout + result.stderr)
    return result.stdout + result.stderr


def consumer_environment(root, dotnet=None, platform=None):
    environment = dict(os.environ)
    for name in ("CMAKE_PREFIX_PATH", "FSNET_SDK_DIR", "FSNET_SDK_PREFIX", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH"):
        environment.pop(name, None)
    environment["GODOT_SILENCE_ROOT_WARNING"] = "1"
    for category in ("CONFIG", "DATA", "CACHE", "STATE"):
        path = root / ("xdg-" + category.lower())
        path.mkdir()
        environment[f"XDG_{category}_HOME"] = str(path)
    runtime = root / "xdg-runtime"
    runtime.mkdir(mode=0o700)
    environment["XDG_RUNTIME_DIR"] = str(runtime)
    for name, directory in (("DOTNET_CLI_HOME", "dotnet-home"), ("NUGET_PACKAGES", "nuget-packages"),
                            ("NUGET_HTTP_CACHE_PATH", "nuget-http"), ("DOTNET_BUNDLE_EXTRACT_BASE_DIR", "dotnet-bundles")):
        path = root / directory
        path.mkdir()
        environment[name] = str(path)
    if dotnet:
        executable = shutil.which(str(dotnet))
        if not executable:
            candidate = Path(dotnet).resolve()
            if candidate.is_file():
                executable = str(candidate)
        if not executable:
            raise ValueError("requested dotnet SDK executable was not found")
        executable = Path(executable).resolve()
        environment["DOTNET_ROOT"] = str(executable.parent)
        architecture = "ARM64" if platform and platform.endswith("arm64") else "X64"
        environment["DOTNET_ROOT_" + architecture] = str(executable.parent)
        return environment, str(executable)
    return environment, None


def verify(archive, godot, mono_godot, dotnet, godot_nuget_source, godot_version):
    with zipfile.ZipFile(archive) as package:
        manifest = json.loads(package.read("addons/fsnet/package-manifest.json"))
        names = package.namelist()
        expected = {"addons/fsnet/" + name for name in manifest["files_sha256"]}
        if set(names) != expected | {"addons/fsnet/package-manifest.json"} or len(set(names)) != len(names):
            raise ValueError("release archive entries do not exactly match its manifest")
        for name, checksum in manifest["files_sha256"].items():
            if hashlib.sha256(package.read("addons/fsnet/" + name)).hexdigest() != checksum:
                raise ValueError("release archive member checksum mismatch")
    with tempfile.TemporaryDirectory(prefix="fsnet-packaged-consumer-") as folder:
        root = Path(folder)
        safe_unpack(archive, root)
        environment, resolved_dotnet = consumer_environment(root, dotnet if manifest["flavor"] == "dotnet" else None,
                                                           manifest["platform"])
        project = '[application]\nconfig/name="FsnetPackageSmoke"\n[rendering]\nrenderer/rendering_method="gl_compatibility"\n'
        flavor = manifest["flavor"]
        if flavor == "gdscript":
            (root / "project.godot").write_text("config_version=5\n" + project)
            (root / "smoke.gd").write_text(GDSCRIPT)
            run([str(godot), "--headless", "--editor", "--path", str(root),
                 "--quit-after", "600", "--max-fps", "60"], root, environment)
            output = run([str(godot), "--headless", "--path", str(root), "--script", "res://smoke.gd"], root, environment)
            if "PASS packaged GDScript addon" not in output:
                raise RuntimeError("packaged GDScript smoke did not announce completion")
        elif flavor == "dotnet":
            if not mono_godot or not godot_nuget_source:
                raise ValueError(".NET archive needs --mono-godot and --godot-nuget-source")
            (root / "project.godot").write_text('config_version=5\n' + project.replace('[rendering]', 'run/main_scene="res://main.tscn"\n[rendering]')
                                               + '\n[dotnet]\nproject/assembly_name="FsnetPackageSmoke"\n')
            (root / "FsnetPackageSmoke.csproj").write_text(
                f'<Project Sdk="Godot.NET.Sdk/{godot_version}"><PropertyGroup><TargetFramework>net8.0</TargetFramework>'
                '<EnableDynamicLoading>true</EnableDynamicLoading><Nullable>enable</Nullable></PropertyGroup></Project>\n')
            (root / "PackagedSmoke.cs").write_text(CSHARP)
            (root / "main.tscn").write_text('[gd_scene load_steps=2 format=3]\n[ext_resource type="Script" path="res://PackagedSmoke.cs" id="1"]\n'
                                           '[node name="Smoke" type="Node"]\nscript=ExtResource("1")\n')
            (root / "NuGet.Config").write_text('<configuration><packageSources><clear/><add key="godot" value="'
                                               + xml.sax.saxutils.escape(str(godot_nuget_source), {'"': '&quot;'})
                                               + '"/></packageSources></configuration>\n')
            run([resolved_dotnet, "restore", "FsnetPackageSmoke.csproj", "--configfile", "NuGet.Config"], root, environment, 120)
            run([resolved_dotnet, "build", "FsnetPackageSmoke.csproj", "--no-restore"], root, environment, 120)
            run([str(mono_godot), "--headless", "--editor", "--path", str(root),
                 "--quit-after", "600", "--max-fps", "60"], root, environment)
            output = run([str(mono_godot), "--headless", "--path", str(root)], root, environment)
            if "PASS packaged .NET addon" not in output:
                raise RuntimeError("packaged .NET smoke did not announce completion")
        else:
            raise ValueError("unknown archive flavor")
    return {"asset": archive.name, "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "platform": manifest["platform"], "flavor": flavor, "status": "passed",
            "source_revision": manifest["source_revision"], "sdk_source_revision": manifest["sdk_source_revision"]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--archives", nargs="+", required=True, type=Path)
    parser.add_argument("--godot", required=True, type=Path)
    parser.add_argument("--mono-godot", type=Path)
    parser.add_argument("--dotnet", default="dotnet")
    parser.add_argument("--godot-nuget-source", type=Path)
    parser.add_argument("--godot-version", default="4.6.3")
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    results = [verify(archive.resolve(), args.godot.resolve(), args.mono_godot.resolve() if args.mono_godot else None,
                      args.dotnet, args.godot_nuget_source.resolve() if args.godot_nuget_source else None, args.godot_version)
               for archive in args.archives]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({"status": "passed", "packages": results}, indent=2) + "\n")
    print(json.dumps({"status": "passed", "packages": results}, indent=2))


if __name__ == "__main__":
    main()
