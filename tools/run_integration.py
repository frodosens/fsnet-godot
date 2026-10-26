#!/usr/bin/env python3
"""Run real Godot Standard and .NET clients using only the precompiled fsnet SDK."""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import shutil
import subprocess
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


def run(command, cwd, env, log, timeout=120):
    try:
        result = subprocess.run(list(map(str, command)), cwd=cwd, env=env, capture_output=True,
                                text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        output = b"".join(part if isinstance(part, bytes) else (part or "").encode()
                          for part in (error.stdout, error.stderr))
        log.write_bytes(output)
        raise
    output = result.stdout + result.stderr
    log.write_text(output)
    if result.returncode or "ERROR:" in output:
        raise RuntimeError(f"Command failed ({result.returncode}): {command[0]}\n{log.read_text()[-12000:]}")
    return output


def copy_addon(project, dotnet=False):
    destination = project / "addons/fsnet"
    shutil.copytree(ROOT / "addons/fsnet", destination,
                    ignore=shutil.ignore_patterns("*.uid", "obj", ".godot"))
    if not dotnet:
        shutil.rmtree(destination / "dotnet", ignore_errors=True)


def nuget_source(mono):
    paths = [mono.parent / "GodotSharp/Tools/nupkgs",
             mono.parent.parent / "Resources/GodotSharp/Tools/nupkgs",
             mono.parent.parent / "GodotSharp/Tools/nupkgs"]
    return next((path for path in paths if path.is_dir()), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--godot", type=Path, required=True)
    parser.add_argument("--mono-godot", type=Path, required=True)
    parser.add_argument("--server", type=Path, required=True)
    parser.add_argument("--platform", required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--dotnet", type=Path, default=Path(shutil.which("dotnet") or "dotnet"))
    parser.add_argument("--project-dir", type=Path, default=ROOT)
    args = parser.parse_args()
    report = args.report.resolve()
    report.parent.mkdir(parents=True, exist_ok=True)
    logs = report.parent / "integration-logs"
    logs.mkdir(exist_ok=True)
    godot, mono, server = (path.resolve() for path in (args.godot, args.mono_godot, args.server))
    dotnet = args.dotnet.resolve()
    for path in (godot, mono, server, dotnet):
        if not path.is_file():
            parser.error(f"Missing executable: {path}")
    outcomes = []
    state = {"status": "failed", "platform": args.platform, "gdscript": False, "dotnet": False,
             "collected_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(), "results": outcomes}
    state["source_revision"] = subprocess.check_output(
        ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    sdk_manifest = ROOT / ".dev/sdk/manifest.json"
    if sdk_manifest.is_file():
        state["sdk_source_revision"] = json.loads(sdk_manifest.read_text())["source_revision"]
    env = {key: value for key, value in os.environ.items() if not key.startswith("FSNET_")}
    temporary_parent = ROOT / ".dev"
    temporary_parent.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="integration-", dir=temporary_parent) as folder:
        temporary = Path(folder)
        env.update(XDG_CONFIG_HOME=str(temporary / "config"), XDG_DATA_HOME=str(temporary / "data"),
                   XDG_CACHE_HOME=str(temporary / "cache"), DOTNET_CLI_HOME=str(temporary / "dotnet-home"),
                   NUGET_PACKAGES=str(temporary / "nuget-packages"), DOTNET_ROOT=str(dotnet.parent),
                   DOTNET_SKIP_FIRST_TIME_EXPERIENCE="1", DOTNET_CLI_TELEMETRY_OPTOUT="1")
        env["PATH"] = str(dotnet.parent) + os.pathsep + env.get("PATH", "")
        for key in ("XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "DOTNET_CLI_HOME", "NUGET_PACKAGES"):
            Path(env[key]).mkdir(parents=True, exist_ok=True)
        token = secrets.token_hex(24)
        env["FSNET_GODOT_TEST_TOKEN"] = token
        env["FSNET_DEMO_TOKEN"] = token
        child = subprocess.Popen([str(server), "--timeout-seconds", "600"], env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, text=True)
        first_line = queue.Queue()
        threading.Thread(target=lambda: first_line.put(child.stdout.readline()), daemon=True).start()
        try:
            announced = first_line.get(timeout=10)
            endpoint = json.loads(announced)
            env["FSNET_GODOT_TEST_PORT"] = str(endpoint["port"])
            env["FSNET_GODOT_TEST_ENTRY_ID"] = endpoint["entry_id"]
            standard = temporary / "standard"
            standard.mkdir()
            copy_addon(standard)
            shutil.copytree(ROOT / "tests/gdscript", standard / "tests/gdscript")
            shutil.copytree(ROOT / "examples/gdscript", standard / "examples/gdscript")
            (standard / "project.godot").write_text((ROOT / "project.godot").read_text())
            # Godot 4.6.3 --import auto-quit can tear down EditorHelp before its
            # deferred extension docs callback. Give the fresh editor a bounded
            # main loop so the callback runs before shutdown; never retry a crash.
            run([godot, "--headless", "--editor", "--path", standard, "--quit-after", "600", "--max-fps", "60"], standard, env,
                logs / "gdscript-import.log")
            output = run([godot, "--headless", "--path", standard, "--script", "res://tests/gdscript/integration.gd"],
                         standard, env, logs / "gdscript-integration.log", timeout=75)
            if "PASS fsnet Godot GDScript integration" not in output:
                raise RuntimeError("GDScript integration exited without its pass marker")
            outcomes.append({"name": "gdscript-native-and-facade-integration", "status": "passed"})
            demo = run([godot, "--headless", "--path", standard, "--", "--scripted", "--manager",
                        "--entry-id=" + endpoint["entry_id"], "--port=" + str(endpoint["port"])],
                       standard, env, logs / "gdscript-example.log", timeout=45)
            if "scene complete" not in demo and "complete" not in demo.lower():
                raise RuntimeError("GDScript scene demo exited without its completion marker")
            outcomes.append({"name": "gdscript-event-driven-example", "status": "passed"})
            state["gdscript"] = True
            managed = temporary / "managed"
            shutil.copytree(ROOT / "examples/csharp", managed,
                            ignore=shutil.ignore_patterns("bin", "obj", ".godot", "*.uid"))
            copy_addon(managed, dotnet=True)
            feed = nuget_source(mono)
            if feed:
                import xml.etree.ElementTree as ET
                config = ET.Element("configuration")
                sources = ET.SubElement(config, "packageSources")
                ET.SubElement(sources, "clear")
                ET.SubElement(sources, "add", key="godot-offline", value=str(feed))
                ET.ElementTree(config).write(managed / "NuGet.Config", encoding="utf-8", xml_declaration=True)
            run([dotnet, "build", "--configuration", "Debug", "--nologo"], managed, env,
                logs / "dotnet-build.log", timeout=120)
            run([mono, "--headless", "--editor", "--path", managed, "--quit-after", "600", "--max-fps", "60"], managed, env,
                logs / "dotnet-import.log", timeout=75)
            for case in ("smoke", "integration"):
                output = run([mono, "--headless", "--path", managed, "--", "--" + case], managed, env,
                             logs / ("dotnet-" + case + ".log"), timeout=60)
                if "PASS fsnet Godot C# facade" not in output:
                    raise RuntimeError("C# test exited without its pass marker: " + case)
                outcomes.append({"name": "dotnet-" + case, "status": "passed"})
            state["dotnet"] = True
            state["status"] = "passed"
        except Exception as error:
            state["error"] = str(error)
            raise
        finally:
            if child.poll() is None:
                child.terminate()
            try:
                out, err = child.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                child.kill()
                out, err = child.communicate(timeout=3)
            (logs / "server.log").write_text((out or "") + (err or ""))
            state["server_exit_code"] = child.returncode
            state["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
            state["native_sha256"] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                       for path in (ROOT / "addons/fsnet/bin").iterdir() if path.suffix in (".so", ".dll", ".dylib")}
            report.write_text(json.dumps(state, indent=2) + "\n")
    print(json.dumps(state, indent=2))


if __name__ == "__main__":
    main()
