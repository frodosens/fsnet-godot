extends Node3D
## Works with fsnet's scene demo protocol: Hub.login, Player.input and Hub.logout.
## Launch with -- --entry-id=<hub-id> --port=<port> [--manager] [--scripted].
## A real game's server list supplies the gateway address and entry entity ID.

const PlayerView = preload("res://examples/gdscript/player_view.gd")

var _views: Dictionary = {}
var _entry_id: String = ""
var _login_id: String = ""
var _player_id: String = ""
var _controlled_id: String = ""
var _next_input: int = 1
var _epoch: int = 0
var _input_busy: bool = false
var _scripted: bool = false
var _scripted_started: bool = false
var _logout_complete: bool = false
var _stopping: bool = false
var _camera: Camera3D
var _settings: Dictionary = {}


func _ready() -> void:
	_camera = Camera3D.new()
	_camera.position = Vector3(0.0, 5.0, 8.0)
	_camera.current = true
	add_child(_camera)
	_camera.look_at(Vector3.ZERO, Vector3.UP)
	var light := DirectionalLight3D.new()
	light.rotation_degrees = Vector3(-60.0, -25.0, 0.0)
	add_child(light)
	var floor := MeshInstance3D.new()
	floor.mesh = PlaneMesh.new()
	floor.scale = Vector3(20.0, 1.0, 20.0)
	floor.position.y = -1.0
	add_child(floor)
	_settings = _configuration()
	_entry_id = str(_settings["entry_id"])
	_scripted = bool(_settings["scripted"])
	Fsnet.connection_state_changed.connect(_connection_changed)
	Fsnet.entity_entered.connect(_entity_entered)
	Fsnet.entity_changed.connect(_entity_changed)
	Fsnet.entity_control_changed.connect(_control_changed)
	Fsnet.entity_left.connect(_entity_left)
	Fsnet.error_occurred.connect(func(message: String) -> void: push_warning(message))
	if _entry_id.is_empty():
		print("fsnet demo: supply -- --entry-id=<Hub EntityId> --port=<port>; A/D moves after possession.")
		return
	var error: int = Fsnet.connect_server(str(_settings["address"]), int(_settings["port"]), _settings["options"])
	if error != OK:
		_fail("Cannot open fsnet connection: " + Fsnet.get_last_error())


func _configuration() -> Dictionary:
	var config := {
		"entry_id": OS.get_environment("FSNET_DEMO_ENTRY_ID"),
		"address": "127.0.0.1",
		"port": 7000,
		"display_name": "GodotPlayer",
		"manager": false,
		"scripted": false,
		"options": {},
	}
	for argument: String in OS.get_cmdline_user_args():
		if argument == "--manager":
			config["manager"] = true
		elif argument == "--scripted":
			config["scripted"] = true
		elif argument.begins_with("--entry-id="):
			config["entry_id"] = argument.trim_prefix("--entry-id=")
		elif argument.begins_with("--address="):
			config["address"] = argument.trim_prefix("--address=")
		elif argument.begins_with("--port="):
			config["port"] = int(argument.trim_prefix("--port="))
		elif argument.begins_with("--name="):
			config["display_name"] = argument.trim_prefix("--name=")
	var names := {"token": "FSNET_DEMO_TOKEN", "ca_file": "FSNET_DEMO_CA_FILE", "certificate_file": "FSNET_DEMO_CERTIFICATE_FILE", "private_key_file": "FSNET_DEMO_PRIVATE_KEY_FILE", "server_name": "FSNET_DEMO_SERVER_NAME"}
	var options: Dictionary = config["options"]
	for key: String in names:
		var value: String = OS.get_environment(names[key])
		if not value.is_empty():
			options[key] = value
	return config


func _connection_changed(state: int, status: int) -> void:
	if state == 2:
		_epoch += 1
		_login(_epoch)
	elif state == 0:
		_epoch += 1
		_controlled_id = ""
		if not _stopping and not _logout_complete:
			print("fsnet disconnected, status=", status)


func _login(epoch: int) -> void:
	_login_id = _entry_id
	if bool(_settings["manager"]):
		var discovered: Dictionary = await Fsnet.call_async(_entry_id, "discover").wait()
		if _stopping or epoch != _epoch:
			return
		if int(discovered["status"]) != 0:
			_fail("Login discovery failed: " + str(discovered["status"]))
			return
		_login_id = (discovered["data"] as PackedByteArray).get_string_from_utf8()
	var reply: Dictionary = await Fsnet.call_async(_login_id, "login", str(_settings["display_name"]).to_utf8_buffer()).wait()
	if _stopping or epoch != _epoch:
		return
	if int(reply["status"]) != 0:
		_fail("Login failed: " + str(reply["status"]))
		return
	_player_id = (reply["data"] as PackedByteArray).get_string_from_utf8()
	print("Player ID: ", _player_id)
	# Notifications can be processed before or after this RPC response. Both
	# paths call the same gate; there is no sleep-and-get possession loop.
	_try_start_scripted()


func _entity_entered(entity: Dictionary) -> void:
	if str(entity.get("type", "")) != "Player" or not bool(entity.get("automatic", false)):
		return
	var id: String = str(entity["id"])
	if not _views.has(id):
		var view: Node3D = PlayerView.new()
		add_child(view)
		_views[id] = view
		print("LocalPlayer " if entity.get("owned") == true else "RemotePlayer ", id)
	_views[id].apply_entity(entity)
	if bool(entity.get("controlled", false)):
		_control_changed(entity)


func _entity_changed(entity: Dictionary) -> void:
	var id: String = str(entity.get("id", ""))
	if _views.has(id):
		_views[id].apply_entity(entity)


func _control_changed(entity: Dictionary) -> void:
	var id: String = str(entity.get("id", ""))
	if bool(entity.get("controlled", false)):
		_controlled_id = id
		var values: Dictionary = entity.get("values", {})
		_next_input = int(values.get("processed_input", 0)) + 1
		print("Camera follows ", id)
		_try_start_scripted()
	elif id == _controlled_id:
		_controlled_id = ""
	if _views.has(id):
		_views[id].apply_entity(entity)


func _entity_left(entity: Dictionary) -> void:
	var id: String = str(entity.get("id", ""))
	if id == _controlled_id:
		_controlled_id = ""
	if _views.has(id):
		_views[id].queue_free()
		_views.erase(id)
	_try_finish_scripted()


func _process(_delta: float) -> void:
	if _views.has(_controlled_id):
		var target: Node3D = _views[_controlled_id]
		_camera.global_position = target.global_position + Vector3(0.0, 5.0, 8.0)
		_camera.look_at(target.global_position, Vector3.UP)


func _unhandled_key_input(event: InputEvent) -> void:
	if not event is InputEventKey or not event.pressed or event.echo or _scripted:
		return
	if event.keycode == KEY_A or event.keycode == KEY_LEFT:
		_move(-1)
	elif event.keycode == KEY_D or event.keycode == KEY_RIGHT:
		_move(1)


func _move(delta: int) -> Dictionary:
	if _controlled_id.is_empty() or _input_busy or _stopping:
		return {"status": 5}
	_input_busy = true
	var epoch: int = _epoch
	var sequence: int = _next_input
	_next_input += 1
	var entity_id: String = _controlled_id
	var payload: PackedByteArray = (str(sequence) + ":" + str(delta)).to_utf8_buffer()
	var reply: Dictionary = await Fsnet.call_async(entity_id, "input", payload, {"request_id": "godot-input-" + str(sequence)}).wait()
	_input_busy = false
	if not _stopping and epoch == _epoch and int(reply["status"]) == 0:
		var entity: Dictionary = Fsnet.get_entity(entity_id)
		print("Confirmed position: ", (entity.get("values", {}) as Dictionary).get("position"))
	return reply


func _try_start_scripted() -> void:
	if _scripted and not _scripted_started and not _player_id.is_empty() and _controlled_id == _player_id:
		_scripted_started = true
		_run_scripted()


func _run_scripted() -> void:
	for delta: int in [2, 3, -1]:
		var reply: Dictionary = await _move(delta)
		if _stopping:
			return
		if int(reply["status"]) != 0:
			_fail("Scripted move failed: " + str(reply["status"]))
			return
	var logout: Dictionary = await Fsnet.call_async(_login_id, "logout").wait()
	if _stopping:
		return
	if int(logout["status"]) != 0:
		_fail("Logout failed: " + str(logout["status"]))
		return
	_logout_complete = true
	_try_finish_scripted()


func _try_finish_scripted() -> void:
	if _scripted and _logout_complete and not _views.has(_player_id):
		print("fsnet GDScript example complete")
		_stopping = true
		Fsnet.disconnect_server()
		get_tree().quit(0)


func _fail(message: String) -> void:
	push_error(message)
	if _scripted:
		_stopping = true
		Fsnet.disconnect_server()
		get_tree().quit(1)


func _exit_tree() -> void:
	_stopping = true
	Fsnet.disconnect_server()
