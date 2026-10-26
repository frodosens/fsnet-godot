extends SceneTree

const Session = preload("res://addons/fsnet/fsnet.gd")

var failures: Array[String] = []
var responses: Dictionary = {}
var entered: Array = []
var controlled: Array = []
var left: Array = []
var api: Session
var entry: String
var address := "127.0.0.1"
var port: int
var options: Dictionary


func _init() -> void:
	_run.call_deferred()


func check(condition: bool, message: String) -> void:
	if not condition:
		failures.append(message)
		push_error(message)


func until(predicate: Callable, message: String, timeout_ms: int = 4000) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while not predicate.call() and Time.get_ticks_msec() < deadline:
		await process_frame
	var passed: bool = predicate.call()
	check(passed, message)
	return passed


func reply(ticket: int) -> Dictionary:
	if ticket <= 0:
		check(false, "request was not accepted: " + api.get_last_error())
		return {"status": -1, "data": PackedByteArray()}
	if not await until(func(): return responses.has(ticket), "request completion deadline"):
		return {"status": -1, "data": PackedByteArray()}
	var result: Dictionary = responses[ticket]
	responses.erase(ticket)
	return result


func _run() -> void:
	port = int(OS.get_environment("FSNET_GODOT_TEST_PORT"))
	entry = OS.get_environment("FSNET_GODOT_TEST_ENTRY_ID")
	options = {"token": OS.get_environment("FSNET_GODOT_TEST_TOKEN")}
	api = root.get_node("Fsnet")
	api.request_completed.connect(func(ticket: int, status: int, data: PackedByteArray):
		responses[ticket] = {"status": status, "data": data})
	api.entity_entered.connect(func(entity: Dictionary): entered.append(entity))
	api.entity_control_changed.connect(func(entity: Dictionary): controlled.append(entity))
	api.entity_left.connect(func(entity: Dictionary): left.append(entity))
	check(api.get_connection_state() == 0, "initial connection state")
	var rejected := api.call_entity(entry, "echo", PackedByteArray())
	check(not responses.has(rejected), "synchronous rejection emitted before method returned")
	check((await reply(rejected))["status"] == 4, "disconnected request rejection")
	check(api.connect_server(address, port, options) == OK, "open fixture connection")
	if not await until(func(): return api.get_connection_state() == 2, "connect deadline"):
		finish()
		return
	check((await reply(api.call_entity("invalid", "echo", PackedByteArray())))["status"] == 5, "invalid EntityId rejection")
	var login: Dictionary = await api.call_async(entry, "login", "GDScript".to_utf8_buffer(), {"request_id": "gdscript-login"}).wait()
	check(login["status"] == 0, "GDScript async login")
	var player_id: String = login["data"].get_string_from_utf8()
	if not await until(func(): return not api.get_controlled_entity().is_empty(), "automatic entity control deadline"):
		finish()
		return
	var player := api.get_entity(player_id)
	check(not entered.is_empty() and not controlled.is_empty(), "presence/control events")
	check(player["automatic"] and player["owned"] == true and player["controlled"], "server ownership metadata")
	check(player["values"]["created"] == 1, "initial confirmed entity properties")
	var original_handle: Dictionary = player["handle"]
	var binary := PackedByteArray([0, 255, 1, 128, 65, 0])
	var result := await reply(api.call_entity(player_id, "echo", binary))
	check(result["status"] == 0 and result["data"] == binary, "binary RPC roundtrip")
	var watched: Dictionary = await api.request_watch("Player", player_id).wait()
	check(watched["status"] == 0 and not watched["entity"].is_empty(), "manual watch completion")
	if watched["status"] == 0:
		check(api.unwatch_entity(watched["entity"]["handle"]), "manual unwatch")
	check(api.get_entity(player_id)["automatic"], "automatic projection survives manual unwatch")
	var codec_failures: Array[String] = await preload("res://tests/gdscript/value_codec.gd").run(api, original_handle)
	for failure in codec_failures:
		check(false, failure)
	var before := api.get_entity(player_id)
	var sequence: int = before["values"]["processed_input"] + 1
	result = await reply(api.call_entity(player_id, "input", (str(sequence) + ":2").to_utf8_buffer()))
	check(result["status"] == 0, "input RPC")
	var after := api.get_entity(player_id)
	check(after["values"]["processed_input"] == sequence, "authoritative input acknowledgement")
	check(is_equal_approx(after["values"]["position"]["x"], before["values"]["position"]["x"] + 2.0), "authoritative movement")
	# A paused game must still drive the native connection and its wall-clock deadlines.
	paused = true
	result = await reply(api.call_entity(player_id, "echo", binary))
	paused = false
	check(result["status"] == 0 and result["data"] == binary, "network polling while SceneTree paused")
	result = await reply(api.call_entity(player_id, "slow", binary, {"timeout_ms": 25}))
	check(result["status"] == 3, "RPC timeout")
	result = await reply(api.call_entity(player_id, "echo", binary))
	check(result["status"] == 0, "connection remains healthy after timeout")
	var pending = api.call_async(player_id, "slow", binary)
	api.disconnect_server()
	check((await pending.wait())["status"] == 4, "disconnect cancels GDScript wait")
	check(api.connect_server(address, port, options) == OK, "immediate reconnect")
	if await until(func(): return api.get_connection_state() == 2, "reconnect deadline"):
		result = await reply(api.call_entity(entry, "login", "GDScript".to_utf8_buffer()))
		check(result["status"] == 0 and result["data"].get_string_from_utf8() == player_id, "stable entity identity after reconnect")
		await until(func(): return not api.get_controlled_entity().is_empty(), "reconnected control")
		check(api.get_entity_by_handle(original_handle).is_empty(), "stale handle lookup")
		result = await reply(api.write_entity(original_handle, {"position": Vector3.ZERO}))
		check(result["status"] == 5, "stale handle write")
		result = await reply(api.call_entity(entry, "logout", PackedByteArray()))
		check(result["status"] == 0, "logout")
		await until(func(): return api.get_entity(player_id).is_empty(), "automatic entity leave")
		check(not left.is_empty() and api.get_controlled_entity().is_empty(), "leave and unpossess")
	api.disconnect_server()
	await reentrant_lifecycle()
	finish()


func reentrant_lifecycle() -> void:
	var temporary := FsnetClient.new()
	root.add_child(temporary)
	temporary.connection_state_changed.connect(func(state: int, _status: int):
		if state == 2:
			# Godot locks the Node while its _process call emits this signal.
			# Scene callbacks release Nodes with queue_free, as in normal gameplay.
			temporary.queue_free())
	check(temporary.open_connection(address, port, options) == OK, "temporary connection")
	var temporary_id := temporary.get_instance_id()
	await until(func(): return not is_instance_id_valid(temporary_id), "queue_free during connection signal")
	var client := FsnetClient.new()
	root.add_child(client)
	var completed := {}
	var transition := {"reopened": false, "ticket": 0}
	client.request_completed.connect(func(ticket: int, status: int, data: PackedByteArray):
		completed[ticket] = {"status": status, "data": data})
	check(client.open_connection(address, port, options) == OK, "reentrancy connection")
	if not await until(func(): return client.get_connection_state() == 2, "reentrancy connect deadline"):
		client.free()
		return
	var login_ticket := client.call_entity(entry, "login", "Reentrant".to_utf8_buffer())
	await until(func(): return completed.has(login_ticket), "reentrancy login")
	await until(func(): return not client.get_controlled_entity().is_empty(), "reentrancy initial control")
	client.entity_left.connect(func(_entity: Dictionary):
		if transition["reopened"]:
			return
		transition["reopened"] = true
		check(client.open_connection(address, port, options) == OK, "reconnect from entity_left")
		transition["ticket"] = client.call_entity(entry, "login", "Reentrant".to_utf8_buffer()))
	client.close_connection()
	if await until(func(): return transition["reopened"] and completed.has(transition["ticket"]), "reentrant new-session request"):
		check(completed[transition["ticket"]]["status"] == 0, "old disconnect cancelled new-session request")
	client.free()


func finish() -> void:
	if is_instance_valid(api):
		api.disconnect_server()
	if failures.is_empty():
		print("PASS fsnet Godot GDScript integration: RPC, entities, codec, handles, cancellation and reentrancy")
	quit(0 if failures.is_empty() else 1)
