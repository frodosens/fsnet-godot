# 构建、验证和发布插件

插件只查找已安装的 `fsnet::client` SDK。插件构建不 checkout、不下载 fsnet 实现源码。
Godot C++ 绑定单独固定到提交与 SHA256；标准版和 .NET 版使用同一个原生模块。

## 依赖版本与资产

- 插件版本：`0.1.0`。
- Godot 标准版 / .NET 版验证版本：`4.6.3`，扩展声明最低 `4.6`。
- fsnet SDK release：`frodosens/fsnet` 的 `sdk-v0.3.1`。
- SDK 资产：`fsnet-sdk-v0.3.1-<platform>.zip` 及 `SHA256SUMS`。
- SDK ZIP 根目录直接包含 `include/`、`lib/`、`bin/`、`licenses/`、`manifest.json`。

平台标识为 `linux-x86_64`、`windows-x86_64`、`macos-x86_64`、`macos-arm64`。
Windows 使用 MinGW64 编译器，并随包携带 SDK 清单中的 C++ 运行库；macOS 两种架构分别构建和执行测试。
平台支持结论以该版本的实际验证报告为准，不能把配置了矩阵等同于已通过矩阵。

## 在自己的工程构建

需要兼容 SDK ABI 的 C++20 编译器、CMake 3.24+、Ninja、Python 3，以及用于 .NET 接入的 .NET 8 SDK。

```sh
python3 tools/fetch_sdk.py --platform linux-x86_64 --prefix .dev/sdk
cmake -S . -B .dev/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/.dev/sdk"
cmake --build .dev/build --parallel 2
```

下载器核对 `SHA256SUMS`、目标平台、规范源码提交和安装布局；`--expected-revision COMMIT`
可进一步锁定 SDK 的准确提交。已有本地 SDK ZIP 可使用 `--archive ZIP --checksums SHA256SUMS`，全程离线安装。
下载器拒绝穿越路径、链接、大小超限和非空安装目录，不覆盖现有 SDK。

SDK 与插件必须使用兼容的标准库 ABI。安装了 SDK 不代表可以任意混用 MSVC、MinGW、不同平台或架构。
无需 Lua；Godot 项目只需要最终插件 ZIP，不需要 SDK、C++ 编译器或 fsnet 实现源码。
.NET 使用者仍需要 Godot .NET 与相应 .NET SDK 来编译自己的 C# 项目。

## 验证与打包

```sh
python3 tools/fetch_godot.py --version 4.6.3 --platform linux-x86_64 --prefix .dev/godot
# tools.json 提供 godot、mono_godot、godot_nuget_source 的准确路径。
python3 tools/run_integration.py --godot /path/to/standard/godot \
  --mono-godot /path/to/mono/godot --server .dev/build/fsnet-godot-test-server \
  --platform linux-x86_64 --report .dev/integration-report.json
```

提交已审阅源码后再生成发行包，打包工具拒绝脏工作区（包括未跟踪源码）。

```sh
python3 tools/package.py --platform linux-x86_64 --sdk-prefix .dev/sdk \
  --bindings-dir .dev/build/_deps/godot-cpp-src --validation .dev/integration-report.json \
  --version 0.1.0 --output-dir dist
python3 tools/verify_package.py --godot /path/to/standard/godot \
  --mono-godot /path/to/mono/godot --godot-nuget-source /path/to/GodotSharp/Tools/nupkgs \
  --archives dist/*.zip --report dist/package-verification.json
```

第一轮验证通过后，工具分别打包 GDScript 与 .NET ZIP。两者都以 `addons/fsnet/` 为根，
包含本平台原生模块、fsnet 运行库、必要的运行时依赖、许可证文本、SDK 元数据、逐文件 SHA256 和验证报告。
.NET ZIP 额外包含 C# facade 源文件；GDScript ZIP 不混入这些 C# 文件。
未验证语言不能打包；未匹配平台的 ELF/PE/Mach-O、缺失依赖、错误 SDK 提交或未包含许可证都会使打包失败。
报告还必须用 `native_sha256` 按文件名记录所有原生二进制的 SHA256；打包时重新核对实际字节，验证后替换模块或 SDK 运行库会被拒绝。

`verify_package.py` 解压到全新的临时 Godot 项目，验证原生类加载和异步请求接口；
.NET 包在新项目内编译并实际运行 facade。这一步不包含 fsnet SDK/头文件，不借用仓库内的 addon。
它与真实网络实体集成测试分别验证“功能”和“用户拿到 ZIP 后可独立接入”。

首次编辑器导入使用 `--headless --editor --quit-after 600 --max-fps 60`，给 Godot 4.6.3 的原生类文档与资源后台扫描留出有界的完成窗口。
工具不使用立即 `--import --quit`，因为该引擎版本在后台文档任务尚未完成时立即退出可能发生退出竞争；实际项目正常运行不需要这个退出参数。
macOS 上该版本还可能在编辑器退出时报告 Android 导出设置读取 `export/android/android_sdk_path` 的 `_EDITOR_GET` 诊断。集成工具仅在编辑器正常退出时允许这一条精确的引擎诊断，并在 `editor_diagnostics` 中记录；游戏运行和其他 ERROR 仍判为失败。
临时项目具有独立可写的 XDG 配置/数据/缓存目录与 .NET/NuGet 缓存；显式 `--dotnet` 同时设置该 SDK 的 `DOTNET_ROOT`，不依赖机器上的另一套 SDK。

原生二进制、Godot/.NET 下载、TLS 测试私钥和本机配置不会提交；包内只允许明确的 addon 源文件、
相应原生库、文档与许可证。应用证书必须由使用者配置，不放入发行包。

## 四目标 CI 与统一发行

`.github/workflows/plugin.yml` 使用以下本机 runner，四个目标分别运行标准 Godot 和 .NET Godot：

| 平台 | Runner |
| --- | --- |
| Linux x86_64 | ubuntu-22.04 |
| Windows x86_64 | windows-2022 / MSYS2 MINGW64 |
| macOS x86_64 | macos-15-intel |
| macOS arm64 | macos-15 |

workflow 只读取已发布 SDK 资产，依次执行构建、真实网络集成、打包和独立 ZIP 消费者验证。
成功资产名为 `fsnet-godot-<platform>`；诊断资产为 `validation-<platform>`。
workflow 支持 `version`、`sdk_tag` 和可选 `sdk_revision` 的手动触发。

下载一个成功 workflow run 的四份插件资产后汇总：

```sh
gh run download RUN_ID --repo frodosens/fsnet-godot --pattern 'fsnet-godot-*' --dir .dev/release-artifacts
python3 tools/collect_release.py --artifacts .dev/release-artifacts --version 0.1.0 --output-dir dist/release
```

汇总器要求四个平台各有两种语言的独立 ZIP 验证，插件与 SDK 提交一致，所有 ZIP 校验和仍然匹配。
它产出八个 ZIP、统一 `SHA256SUMS` 和 `release-manifest.json`，供发布到 GitHub Release `v0.1.0`。
正式发布只使用这些验证成功的资产；不能用同一 Linux 二进制改名冒充 Windows/macOS。

也可以手动触发 `.github/workflows/publish.yml`，输入成功的 `Desktop plugin` run ID、准确源码提交和版本。
发布 job 先核对该 run 来自本仓库、成功完成、运行了指定 workflow、head SHA 完全匹配，再在 CI 内部下载和验证四份目标资产。
因此本机不必直接访问 GitHub Actions 的底层 blob 存储。只有发布 job 获得 contents write，构建 job 保持只读。
已有 tag 或 release 会被拒绝，不覆盖历史发行。新 release 先保持 draft，八份 ZIP、校验和和来源清单全部上传后才公开。
