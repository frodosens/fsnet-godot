# GDScript 接入

fsnet 插件使用 Godot 4 GDExtension，标准版和 .NET 版 Godot 加载同一原生核心，不使用 Lua。
安装匹配系统和架构的发行包，将 `addons/fsnet` 放到项目根目录，然后启用
**Project → Project Settings → Plugins → fsnet**。插件会添加 `Fsnet` Autoload。
如果该名称已被其他脚本占用，插件不会覆盖它；请先解决名称冲突或自己实例化 `FsnetClient`。

## 连接与实体 RPC

服务器列表/可信配置提供数字 IP、端口和入口 EntityId。入口通常是 Manager，也可以直接是 LoginEntity/Hub。

```gdscript
func _ready() -> void:
    Fsnet.connection_state_changed.connect(_on_connection_state)
    var error := Fsnet.connect_server("127.0.0.1", 7000, {
        "server_name": "gateway.example",
        "ca_file": "user://credentials/ca.pem",
        "certificate_file": "user://credentials/client.pem",
        "private_key_file": "user://credentials/client.key",
    })
    if error != OK:
        push_error(Fsnet.get_last_error())

func _on_connection_state(state: int, _status: int) -> void:
    if state == 2:
        _login()

func _login() -> void:
    var discovered: Dictionary = await Fsnet.call_async(manager_id, "discover").wait()
    if discovered.status != 0:
        return
    var login_id := (discovered.data as PackedByteArray).get_string_from_utf8()
    var result: Dictionary = await Fsnet.call_async(login_id, "login", "Alice".to_utf8_buffer()).wait()
    if result.status == 0:
        var player_id := (result.data as PackedByteArray).get_string_from_utf8()
        print("Player: ", player_id)
```

`open_connection` 的返回值是 Godot Error；RPC、write 和 watch 的完成状态是 fsnet Status：
0 成功、1 不存在、2 过载、3 超时、4 断线、5 拒绝、6 内部错误。两者不能混用。
TLS 路径应指向实际文件；打入导出项目 PCK 的文件不能直接交给 fsnet 的文件型 TLS 接口。

`call_async`、`request_write`、`request_watch` 返回 `Request`。`await request.wait()`
在完成后返回字典，即使失败或断线也是一次完成，不抛脚本异常。`data` 总是 PackedByteArray，保留 NUL 和任意二进制字节。
watch 结果的 `entity` 在失败或取消时为空字典；使用实体前先检查 `status`。
Node 退出树、`disconnect_server`、`connect_server` 替换连接或服务器断开时，旧请求以 disconnected 完成。
超时由原生 SDK 处理；`request_id` 仅在应用明确需要服务端幂等重试时填写，SDK 不自动重放业务 RPC。
本地 ticket 只用于匹配完成事件，不能把它当作持久化幂等键。
`Fsnet`、原生客户端和这些 Request 帮助器均在 Godot 主线程调用；线程池中的工作应先转回主线程再访问场景对象或 SDK。

## 实体、镜头与输入

```gdscript
func _ready() -> void:
    Fsnet.entity_entered.connect(_on_entity_entered)
    Fsnet.entity_changed.connect(_on_entity_changed)
    Fsnet.entity_control_changed.connect(_on_control_changed)
    Fsnet.entity_left.connect(_on_entity_left)

func _on_entity_entered(entity: Dictionary) -> void:
    if not entity.automatic:
        return
    # owned 决定本地/远程表现，controlled 决定当前是否接受输入。
    var scene: PackedScene = local_player_scene if entity.owned == true else remote_player_scene
    var view := scene.instantiate()
    add_child(view)
    views[entity.id] = view

func _on_entity_changed(entity: Dictionary) -> void:
    var position: Dictionary = entity.values.position
    views[entity.id].position = Vector3(position.x, position.y, position.z)

func _on_control_changed(entity: Dictionary) -> void:
    if entity.controlled:
        input_target = entity.id
        camera_target = views.get(entity.id)
    elif input_target == entity.id:
        input_target = ""
        camera_target = null

func _on_entity_left(entity: Dictionary) -> void:
    if views.has(entity.id):
        views[entity.id].queue_free()
        views.erase(entity.id)
```

RPC 返回 Player ID 和客户端处理实体出现/操控通知是独立流程。`examples/gdscript/main.gd`
同时从登录结果与 control 事件检查准备条件，不使用 `repeat/get/sleep(1)` 轮询。
信号回调适合同步更新表现和输入目标；需要等待的 RPC 放在独立协程里，避免回调阻塞客户端推进。

## 属性写入

```gdscript
var entity: Dictionary = Fsnet.get_entity(player_id)
var result: Dictionary = await Fsnet.request_write(entity.handle, {
    "position": Vector3(1.0, 2.0, 3.0)
}, {
    "expected_revision": entity.revision,
    "request_id": "explicit-application-operation-key"
}).wait()
```

只有服务端允许客户端写入的属性才会成功。确认快照不在本地乐观修改；写入继续使用 fsnet 的差量生成与服务端校验。
`self.position.x = ...` 是普通 Godot 场景节点属性修改，不会隐式变成网络写入，请调用上述接口。
handle 为不透明字典，包含实例/连接信息；重连或换 Client 后应重新获取，不能修改或复用旧 handle。
EntityId 与 revision 用字符串保留精度；revision 可超过 GDScript 有符号 int64 的范围。

普通 Dictionary 对应 struct，Array 对应数组/list/set 的值，map 使用 `FsnetMap` 保留线上的 map 类型：

```gdscript
var labels := FsnetMap.from_dictionary({"daily": true, "season": false})
var result: Dictionary = await Fsnet.request_write(entity.handle, {"labels": labels}).wait()
```

Vector2/3/4、Quaternion、Color 会转换为命名结构；Transform3D/Projection 转换为四行数组。
服务器值仍按 Dictionary/Array 返回；map 返回 FsnetMap，其 `entries` 可读取成 Dictionary。
不会根据字段名猜测它是不是游戏中的 Vector3/背包/任务。

## 可运行示例

发行插件 ZIP 用于安装 `addons/fsnet`，不包含整个演示工程；下列命令在插件仓库的根目录运行。
演示只需要此插件仓库及预编译 SDK/插件，不需要 fsnet 的实现源码。

仓库项目的主场景是 `examples/gdscript/main.tscn`。它使用当前 fsnet scene 示例的协议：
Hub.login → Player.input → Hub.logout，Player.input 为 `序号:位移` UTF-8 字节。
绘制蓝色本地玩家、橙色远端玩家，以 control 事件绑定镜头，A/D 或左右方向键发送移动输入。
示例只显示服务器确认的位置，不实现客户端预测。

```sh
godot --path . -- --entry-id=0000000000000001 --address=127.0.0.1 --port=7000
# 入口是 Manager 时先 discover；这要求服务端使用相应 Manager 协议：
godot --path . -- --entry-id=<manager-id> --port=7000 --manager
# 与临时 scene fixture 集成验收：移动三次、登出并自动退出。
godot --headless --path . -- --entry-id=<hub-id> --port=<port> --scripted
```

生产客户端通过服务端配置获取真实入口；上述 EntityId 只是命令格式示意。
演示还读取 FSNET_DEMO_ENTRY_ID、FSNET_DEMO_TOKEN、FSNET_DEMO_CA_FILE、
FSNET_DEMO_CERTIFICATE_FILE、FSNET_DEMO_PRIVATE_KEY_FILE、FSNET_DEMO_SERVER_NAME。
本地明文只用于 loopback 临时测试。发行包支持范围和已验证平台以根 README 和 release notes 为准。
