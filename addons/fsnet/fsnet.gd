extends FsnetClient
## Optional GDScript autoload. All networking remains in the native FsnetClient.
## Use connect_server/disconnect_server with these await helpers so replacing a
## connection cancels the previous session's pending GDScript requests.

enum RpcStatus {
	SUCCESS = 0,
	NOT_FOUND = 1,
	OVERLOADED = 2,
	TIMEOUT = 3,
	DISCONNECTED = 4,
	REJECTED = 5,
	INTERNAL = 6,
}

const MAX_PENDING_REQUESTS := 1024


class Request extends RefCounted:
	signal completed(result: Dictionary)
	var done: bool = false
	var result: Dictionary = {}
	var ticket: int = 0


	func wait() -> Dictionary:
		if done:
			return result
		return await completed


	func _finish(value: Dictionary) -> void:
		if done:
			return
		done = true
		result = value
		completed.emit(result)


var _requests: Dictionary = {}
var _stopping: bool = false
var _changing_connection: bool = false


func _enter_tree() -> void:
	_stopping = false
	if not request_completed.is_connected(_on_request_completed):
		request_completed.connect(_on_request_completed)
	if not watch_completed.is_connected(_on_watch_completed):
		watch_completed.connect(_on_watch_completed)
	if not connection_state_changed.is_connected(_on_connection_state_changed):
		connection_state_changed.connect(_on_connection_state_changed)


func _exit_tree() -> void:
	_stopping = true
	_cancel_requests("The fsnet node left the scene tree.")
	close_connection()


func connect_server(address: String, port: int, options: Dictionary = {}) -> int:
	if _stopping or _changing_connection:
		return ERR_BUSY
	_changing_connection = true
	_cancel_requests("The fsnet connection was replaced.")
	close_connection()
	var error: int = open_connection(address, port, options)
	_changing_connection = false
	return error


func disconnect_server() -> void:
	if _changing_connection:
		return
	_changing_connection = true
	_cancel_requests("The fsnet connection was closed.")
	close_connection()
	_changing_connection = false


func call_async(entity_id: String, method: String, payload: PackedByteArray = PackedByteArray(), options: Dictionary = {}) -> Request:
	var rejected: Request = _rejection_if_unavailable()
	if rejected != null:
		return rejected
	return _track(call_entity(entity_id, method, payload, options), "request")


func request_write(handle: Dictionary, values: Dictionary, options: Dictionary = {}) -> Request:
	var rejected: Request = _rejection_if_unavailable()
	if rejected != null:
		return rejected
	return _track(write_entity(handle, values, options), "request")


func request_watch(entity_type: String, entity_id: String, timeout_ms: int = 3000) -> Request:
	var rejected: Request = _rejection_if_unavailable()
	if rejected != null:
		return rejected
	return _track(watch_entity(entity_type, entity_id, timeout_ms), "watch")


func _rejection_if_unavailable() -> Request:
	if _stopping or _changing_connection:
		return _failed_request(RpcStatus.DISCONNECTED, "The fsnet connection is closing.")
	if _requests.size() >= MAX_PENDING_REQUESTS:
		return _failed_request(RpcStatus.OVERLOADED, "Too many pending GDScript requests.")
	return null


func _failed_request(status: int, message: String) -> Request:
	var request := Request.new()
	request._finish({"ticket": 0, "status": status, "data": PackedByteArray(), "entity": {}, "error": message})
	return request


func _track(ticket: int, kind: String) -> Request:
	if ticket <= 0:
		return _failed_request(RpcStatus.REJECTED, get_last_error())
	var request := Request.new()
	request.ticket = ticket
	_requests[ticket] = {"request": request, "kind": kind}
	return request


func _on_request_completed(ticket: int, status: int, data: PackedByteArray) -> void:
	if not _requests.has(ticket):
		return
	var pending: Dictionary = _requests[ticket]
	if pending["kind"] != "request":
		return
	_requests.erase(ticket)
	var request: Request = pending["request"]
	request._finish({"ticket": ticket, "status": status, "data": data})


func _on_watch_completed(ticket: int, status: int, entity: Dictionary) -> void:
	if not _requests.has(ticket):
		return
	var pending: Dictionary = _requests[ticket]
	if pending["kind"] != "watch":
		return
	_requests.erase(ticket)
	var request: Request = pending["request"]
	request._finish({"ticket": ticket, "status": status, "data": PackedByteArray(), "entity": entity})


func _on_connection_state_changed(state: int, _status: int) -> void:
	# A close followed immediately by open can leave the previous connection's
	# terminal notification queued. It must not cancel the new session's calls.
	if state == 0 and get_connection_state() == 0:
		_cancel_requests("The fsnet server disconnected.")


func _cancel_requests(message: String) -> void:
	var pending: Array = _requests.values()
	_requests.clear()
	for item: Dictionary in pending:
		var request: Request = item["request"]
		request._finish({"ticket": request.ticket, "status": RpcStatus.DISCONNECTED, "data": PackedByteArray(), "entity": {}, "error": message})
