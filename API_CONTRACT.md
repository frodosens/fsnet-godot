# Native adapter contract (v0.1.0)

One `FsnetClient : godot::Node` uses installed `fsnet::Client` and polls automatically
in `_process` (configurable `auto_poll`, explicit `poll()` also available). The
same native class is consumed by GDScript and the C# facade. No Lua dependency.
Target Godot 4.6+, release targets Windows x86_64, Linux x86_64, macOS x86_64/arm64.
Each target needs its own tested precompiled fsnet SDK. API strings use snake_case.

## Methods

- `open_connection(address: String, port: int, options: Dictionary = {}) -> int`:
  Godot Error code; numeric IP only. Options: token, ca_file, certificate_file,
  private_key_file, server_name, connect_timeout_ms, request_timeout_ms. TLS
  paths are actual filesystem paths (globalize user:// and unpacked res://).
  No production credentials in examples. Public endpoints require mutual TLS.
- `close_connection()`; `poll()`; `set_auto_poll(bool)`; `get_auto_poll() -> bool`.
- `get_connection_state() -> int` (0 disconnected, 1 connecting, 2 connected).
- `get_last_error() -> String`; `get_stats() -> Dictionary`.
- `call_entity(entity_id: String, method: String, payload: PackedByteArray,
  options: Dictionary = {}) -> int`: local positive request ticket. Options:
  timeout_ms, request_id. `request_id` is the server idempotency key; local ticket
  is only for matching signals and must not be reused as an idempotency promise.
  Do not override Godot Node's unrelated built-in `rpc` multiplayer method.
- `watch_entity(type: String, entity_id: String, timeout_ms: int = 3000) -> int`.
- `write_entity(handle: Dictionary, values: Dictionary,
  options: Dictionary = {}) -> int`: options expected_revision (decimal String
  or nonnegative int), timeout_ms, request_id. Values are final top-level values;
  the native SDK generates shared field/container deltas and waits for confirmation.
- `unwatch_entity(handle: Dictionary) -> bool`.
- `get_entity(entity_id: String) -> Dictionary` (empty if absent).
- `get_entity_by_handle(handle: Dictionary) -> Dictionary` (empty if stale).
- `get_entities() -> Array` of entity dictionaries.
- `get_controlled_entity() -> Dictionary` (empty if none).

Entity dictionary: id, type, handle, revision (decimal String), values (Dictionary),
automatic (bool), owned (bool or null), controlled (bool). Handle is opaque:
session, generation, subscription (canonical decimal Strings), scoped to this
FsnetClient instance and connection session. Compare session before touching the
SDK. Never silently reinterpret an old handle after reconnect or a new Client.

## Signals

- `connection_state_changed(state: int, status: int)`.
- `entity_entered(entity: Dictionary)`.
- `entity_changed(entity: Dictionary)`.
- `entity_control_changed(entity: Dictionary)`.
- `entity_left(entity: Dictionary)` (may contain metadata without values).
- `request_completed(ticket: int, status: int, data: PackedByteArray)` for RPC/write.
- `watch_completed(ticket: int, status: int, entity: Dictionary)`.
- `error_occurred(message: String)`.

Status is fsnet's 0 ok, 1 not_found, 2 overloaded, 3 timeout, 4 disconnected,
5 rejected, 6 internal. It is distinct from Godot Error used by open_connection.
Every accepted local ticket eventually completes while the Node lives; invalid
arguments enqueue a rejected completion. Completion is never emitted before the
request method returns, including native synchronous failures. Do not call Lua
or GDScript while fsnet is polling. Copy/own queued callback data. Reconnect and
queue_free from signals must not emit stale events or dereference a freed Node.
Ticket 0 means the request could not be admitted (capacity or wrong thread), so
no completion signal follows; language facades return an immediate failure.

## Values

bool/int64/finite double/String, recursive Dictionary (struct), Array, Vector2/3/4,
Quaternion, Color, Projection/Transform3D map to common fsnet values. Matrices use
four row arrays; Godot basis/Projection columns must be transposed into row order.
Server values decode to Dictionary/Array; no inference of gameplay types.
`FsnetMap : RefCounted` explicitly wraps string-key maps. Static
`FsnetMap.from_dictionary(entries)` returns a wrapper, property `entries` is a
Dictionary. Nested wrappers preserve map-vs-struct wire types. Native maps decode
as FsnetMap, so unchanged values roundtrip without losing their variant tag.
PackedByteArray values represent binary strings; valid UTF-8 non-NUL native
strings may decode as String, other native byte strings stay PackedByteArray.
RPC payloads are always PackedByteArray and preserve NUL/arbitrary bytes.

Conversion limits: depth 8, total 4096 value nodes, 256 members per container,
127 UTF-8 bytes/key (nonempty, no NUL), 65536 bytes/string, 1 MiB wire/value budget.
Reject unsupported objects, cycles/excess depth, non-string Dictionary keys,
nonfinite numbers and excessive aggregate bytes before building native graphs.

## Scope

Client adapter only: connection/RPC/automatic presence/control/confirmed values/
subscriptions/writes. Server authority remains in fsnet; no optimistic mutation
of confirmed state. Lua prediction/reducers are not reimplemented here; no
claim of cross-language automatic prediction. Desktop support requires target
validation; mobile/web binaries are outside this release.
