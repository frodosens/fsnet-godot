# fsnet-godot

Godot 4 的 fsnet 客户端插件。GDScript 和 C# 共用一个 GDExtension 原生核心，提供实体 RPC、自动实体出现/离开、连接所有权与操控通知、属性订阅和差量写入。无需 Lua，也无需 fsnet 实现源码。

[下载发行包](https://github.com/frodosens/fsnet-godot/releases) · [GDScript API](docs/GDSCRIPT.md) · [C# API](docs/CSHARP.md) · [原生接口约定](API_CONTRACT.md) · [构建与发布](docs/RELEASE.md)

## 安装

使用 Godot **4.6+**；当前验证版本为 **4.6.3**。下载匹配平台与语言的 ZIP，把其中的 `addons/fsnet` 解压到自己的项目根目录。

| 平台 | 发行包平台标识 |
| --- | --- |
| Windows x86_64 | `windows-x86_64` |
| Linux x86_64 | `linux-x86_64` |
| macOS Intel | `macos-x86_64` |
| macOS Apple Silicon | `macos-arm64` |

- **GDScript**：选择 `gdscript` 包，在 Project Settings → Plugins 启用 fsnet，插件会添加 `Fsnet` Autoload。
- **C#**：选择 `dotnet` 包，使用 Godot .NET 和 .NET 8 SDK，将 `Fsnet.Godot.FsnetConnection` 加入场景树。.NET 包同时保留 GDScript 接口。

每个包都包含对应平台的原生扩展、fsnet 运行库、依赖许可证及验证报告。使用者无需安装 C++ 工具链或下载 fsnet SDK；不要混用不同版本的原生库。

## GDScript

入口 EntityId 来自可信的区服列表或应用配置。连接完成后向入口实体登录，服务端创建并展示 Player 后，客户端自动收到实体和操控事件。

```gdscript
extends Node

@export var entry_id: String

func _ready() -> void:
    Fsnet.connection_state_changed.connect(_on_connection_state)
    Fsnet.entity_control_changed.connect(_on_control_changed)
    var error := Fsnet.connect_server("127.0.0.1", 7000)
    if error != OK:
        push_error(Fsnet.get_last_error())

func _on_connection_state(state: int, _status: int) -> void:
    if state == 2:
        var login: Dictionary = await Fsnet.call_async(
            entry_id, "login", "Alice".to_utf8_buffer()).wait()
        if login.status == 0:
            print("Player ID: ", login.data.get_string_from_utf8())

func _on_control_changed(entity: Dictionary) -> void:
    if entity.controlled:
        print("将镜头和输入目标绑定到 ", entity.id)
```

RPC 与实体通知独立到达，游戏通过 `entity_entered`、`entity_changed`、`entity_control_changed`、`entity_left` 管理场景和镜头。EntityId、版本号与不透明 handle 保留字符串形式，重连后重新获取 handle。

```gdscript
var entity: Dictionary = Fsnet.get_controlled_entity()
var result: Dictionary = await Fsnet.request_write(entity.handle, {
    "position": Vector3(1, 2, 3),
}, {"expected_revision": entity.revision}).wait()
```

属性写入由服务端权限和校验决定，确认后更新客户端视图；共享 fsnet SDK 的字段/容器差量逻辑。修改 Godot 场景 Node 的 `position` 不会隐式发网络请求。

## C#

```csharp
using Fsnet.Godot;
using Godot;
using System;
using System.Text;
using System.Threading.Tasks;

public partial class LoginExample : Node
{
    [Export] public string EntryId { get; set; } = ""; // 可信区服配置
    private FsnetConnection connection = null!;

    public override void _Ready()
    {
        connection = new FsnetConnection();
        AddChild(connection);
        connection.ConnectionStateChanged += (state, status) =>
        {
            if (state == (long)FsnetConnectionState.Connected)
                _ = LoginAsync(); // Task 内部处理全部异常
        };
        connection.EntityControlChanged += entity =>
        {
            if (entity["controlled"].AsBool()) GD.Print("操控实体: ", entity["id"]);
        };
        if (connection.OpenConnection("127.0.0.1", 7000) != Error.Ok)
            GD.PushError(connection.LastError);
    }

    private async Task LoginAsync()
    {
        try
        {
            var login = await connection.RpcAsync(EntryId, "login", Encoding.UTF8.GetBytes("Alice"));
            if (login.IsSuccess) GD.Print("Player ID: ", Encoding.UTF8.GetString(login.Data));
            else GD.PushError("登录失败: " + login.Status);
        }
        catch (OperationCanceledException) { } // 断线或 Node 释放
        catch (Exception error) { GD.PushError(error.Message); }
    }
}
```

C# 使用 `Task` / `CancellationToken`；GDScript 使用可 await 的 Request。原生网络在 Godot 主线程自动推进，完成信号经过队列分发，支持信号中关闭、重连或释放 Node。请求、事件队列和数据转换都有明确上限。

## 数据和连接

- 支持结构 Dictionary、Array、显式 `FsnetMap`、二进制字节，以及 Vector2/3/4、Quaternion、Color、Transform3D、Projection 的写入转换。读取保留公共结构表示。
- `open_connection` 返回 Godot Error；RPC/write/watch 返回独立的 fsnet Status，详见 API 文档。
- 连接使用数字 IP。示例明文连接限本机 loopback；公共网络使用 mTLS，通过连接 options 提供 CA、客户端证书、私钥与服务端名称。证书需要实际文件路径。
- 本地取消等待不会撤销服务器已经执行的操作。需要业务重试时使用稳定的 `request_id` 并遵守服务端幂等约定。
- 当前版本提供服务器确认状态；不实现 Lua 预测 reducer。移动端和 Web 不在本次桌面发行范围内。

## 示例与开发

`examples/gdscript` 演示登录、自动实体、Local/Remote 表现、操控镜头、移动与登出；根项目可以直接打开。`examples/csharp` 提供独立 .NET 场景。两者都使用实体 RPC，入口 ID 通过参数或配置传入。

开发插件只依赖**预编译 fsnet C++ SDK**，不拉取 fsnet 实现源码：

```sh
python3 tools/fetch_sdk.py --platform linux-x86_64 --prefix .dev/sdk
cmake -S . -B .dev/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$PWD/.dev/sdk"
cmake --build .dev/build --parallel 2
```

C++20 编译器必须匹配 SDK ABI；Windows SDK 使用 MinGW64。Godot C++ 绑定固定提交和 SHA256。CI 在四个原生平台分别运行标准版与 .NET 版 Godot，测试真实网络实体流程，再用发行 ZIP 创建全新项目验证独立安装。

插件使用 [MIT License](LICENSE)。随包 fsnet 与其他依赖的许可证和源码位置见发行包内 `licenses/` 和 `THIRD_PARTY_NOTICES.md`。
