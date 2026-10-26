# Godot .NET / C#

`FsnetConnection` 是 GDExtension 原生 `FsnetClient` 的薄封装。它不嵌入 Lua，也不重复实现 RPC、属性同步或差量协议。
安装对应平台的 .NET 插件包；Godot 必须使用 .NET 版本，工程需要兼容的 .NET 8 SDK。
标准 Godot 使用同一个原生二进制和 GDScript 接口。

把 `addons/fsnet` 复制进工程后，将下面的 `LoginExample.cs` Node 加入场景树，并配置入口 EntityId。
先注册连接事件，再发起连接；只有收到 Connected 才开始登录。异步工作使用有完整异常处理的 Task：

```csharp
using Fsnet.Godot;
using Godot;
using System;
using System.Text;
using System.Threading.Tasks;

public partial class LoginExample : Node
{
    [Export] public string EntryId { get; set; } = "";
    private FsnetConnection connection = null!;

    public override void _Ready()
    {
        connection = new FsnetConnection();
        AddChild(connection);
        connection.ConnectionStateChanged += (state, status) =>
        {
            if (state == (long)FsnetConnectionState.Connected) _ = LoginAsync();
            else if (state == (long)FsnetConnectionState.Disconnected && status != 0)
                GD.PushError("连接失败: " + (FsnetStatus)status);
        };
        connection.EntityControlChanged += entity =>
        {
            if (entity["controlled"].AsBool()) GD.Print("镜头跟随 ", entity["id"]);
        };
        var error = connection.OpenConnection("203.0.113.10", 7000,
            new Godot.Collections.Dictionary
            {
                ["ca_file"] = "user://pki/ca.pem",
                ["certificate_file"] = "user://pki/player.pem",
                ["private_key_file"] = "user://pki/player.key",
                ["server_name"] = "gateway.example.com"
            });
        if (error != Error.Ok) GD.PushError(connection.LastError);
    }

    private async Task LoginAsync()
    {
        try
        {
            var login = await connection.RpcAsync(EntryId, "login", Encoding.UTF8.GetBytes("Alice"));
            if (login.IsSuccess) GD.Print("Player ID: ", Encoding.UTF8.GetString(login.Data));
            else GD.PushError("登录失败: " + login.Status);
        }
        catch (OperationCanceledException) { } // 关闭、断线或移出/释放 Node
        catch (Exception error) { GD.PushError(error.Message); }
    }
}
```

示例地址仅为文档占位。TLS 文件需要部署为实际操作系统文件；public TCP 要求 mTLS。`token` 仅用于内部额外认证或 loopback 示例，不代表账号身份。

## API

- `RpcAsync(id, method, byte[], options, CancellationToken)`：返回 `FsnetResult`，含 `Status`、`Data`、`IsSuccess`。
- `WatchEntityAsync(type, id, timeoutMs, CancellationToken)`：返回 `FsnetWatchResult`，成功时 `Entity` 包含 opaque `handle`。
- `WriteEntityAsync(handle, values, options, CancellationToken)`：共享原生属性差量/确认路径。
- `GetEntity`、`GetEntityByHandle`、`GetControlledEntity`、`GetEntities` 读取确认视图。
- `UnwatchEntity(handle)` 释放订阅。
- `CloseConnection()` 取消未完成的托管等待；重新打开已断开的会话会取消残留等待。连接中/已连接时重复 Open 返回 `AlreadyInUse`，保留现有请求。
- `ConnectionStateChanged`、`EntityEntered`、`EntityChanged`、`EntityControlChanged`、`EntityLeft`、`ErrorOccurred` 是 C# 和 Godot 信号。
- 自动 poll 默认开启；显式 `AutoPoll = false` 后由 Godot 主线程调用 `Poll()`。
- `PendingRequestCount` 是托管等待槽位数，包含尚在途但本地 Task 已取消的请求。

所有公开方法必须在该 Node 的 Godot 主线程、已进入场景树后调用。不要使用 `ConfigureAwait(false)` 后直接访问 Godot；将工作结果调度回主线程。
`CancellationToken` 可以从其他 CLR 线程取消：回调只触碰线程安全的托管 Task，清理在原生回复/超时到来或主线程关闭连接时进行。
本地 Task 取消后仍保留有界 ticket，直到尚在途的原生操作结束；它仍占用等待额度，避免游戏暂停期间的清理队列和过量接单。
取消 watch 的在途请求若随后订阅成功，封装会自动 unwatch 其迟到 handle，避免产生调用方无法释放的隐形订阅。
取消本地等待不等于撤销服务器已执行或已提交的 RPC；需要重试时使用稳定的 `request_id` 并遵守服务端幂等合同。
传输断线可返回 `Disconnected`，或随连接生命周期取消 Task；调用方应处理 `OperationCanceledException`。
移出/释放 Node 会取消等待并释放原生连接。单个 wrapper 最多保留 1024 个等待，超过时明确返回 `Overloaded`。

`Dictionary` 表示 struct，`Array` 表示数组；显式 map 使用 `connection.MapValue(entries)`，返回原生 `FsnetMap`。
服务器 map 返回 `FsnetMap`，可通过 `Call("get_entries").AsGodotDictionary()` 读取。`Vector3` 和 `Transform3D` 支持写入；读取值保留公共 Dictionary/Array 表示，不猜测游戏对象类型。
RPC 与二进制属性使用 `byte[]`，保持 NUL 和任意字节。实体 ID、revision 和 handle 字段应按 SDK 给出的字符串保留；不要转为有精度损失的浮点数。

## 示例与验证

`examples/csharp` 是 .NET 工程。先把发行包的 `addons/fsnet` 放到该工程下，运行 `dotnet build`，再使用对应版本的 Godot .NET 打开。
无服务端冒烟：`Godot --headless --path PROJECT -- --smoke`。
真实集成：设置 `FSNET_GODOT_TEST_PORT`、`FSNET_GODOT_TEST_ENTRY_ID`、`FSNET_GODOT_TEST_TOKEN`，再运行 `--integration`。
项目验证工具会准备共同的 C++ 服务器 fixture、复制插件并执行这些命令；它不要求消费者取得 fsnet 实现源码。

集成覆盖连接、登录、自动实体/操控、二进制 RPC、map/vec3/矩阵属性确认、输入序号、超时、关闭取消和重连后的旧句柄拒绝。
Lua 的预测 reducer 不在此插件实现范围内。
