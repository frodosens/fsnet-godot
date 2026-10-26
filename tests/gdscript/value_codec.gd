extends RefCounted
## Integration checks against the SDK-only Player fixture described in API_CONTRACT.
## run() returns failure strings; an empty result means all checks completed.


static func _wait_ticket(client: FsnetClient, ticket: int) -> Dictionary:
	if ticket <= 0:
		return {"status": -1, "error": "request was not admitted"}
	var result := {"done": false, "status": -1, "data": PackedByteArray()}
	var receive := func(completed: int, status: int, data: PackedByteArray) -> void:
		if completed == ticket:
			result["done"] = true
			result["status"] = status
			result["data"] = data
	client.request_completed.connect(receive)
	var deadline := Time.get_ticks_msec() + 3000
	while not result["done"] and Time.get_ticks_msec() < deadline:
		if not is_instance_valid(client) or client.get_tree() == null:
			break
		client.poll()
		if not result["done"]:
			await client.get_tree().process_frame
	if is_instance_valid(client) and client.request_completed.is_connected(receive):
		client.request_completed.disconnect(receive)
	if not result["done"]:
		result["error"] = "completion deadline exceeded"
	return result


static func _write(client: FsnetClient, handle: Dictionary, values: Dictionary) -> Dictionary:
	return await _wait_ticket(client, client.write_entity(handle, values))


static func _check(condition: bool, message: String, failures: Array[String]) -> void:
	if not condition:
		failures.append(message)


static func _rejected(client: FsnetClient, handle: Dictionary, values: Dictionary,
		message_fragment: String, failures: Array[String]) -> void:
	var before := client.get_entity_by_handle(handle)
	var result := await _write(client, handle, values)
	_check(result["status"] == 5, "invalid value was not rejected: " + message_fragment, failures)
	_check(client.get_last_error().contains(message_fragment),
			"conversion did not report its boundary: " + message_fragment, failures)
	var after := client.get_entity_by_handle(handle)
	_check(not before.is_empty() and not after.is_empty() and before["revision"] == after["revision"],
			"rejected conversion changed confirmed revision", failures)


static func run(client: FsnetClient, handle: Dictionary) -> Array[String]:
	var failures: Array[String] = []
	var initial := client.get_entity_by_handle(handle)
	if initial.is_empty():
		failures.append("codec fixture handle is not live")
		return failures
	var writable := ["position", "matrix", "items", "blob", "nested"]
	for field in writable:
		if not initial["values"].has(field):
			failures.append("codec fixture is missing " + field)
	if not failures.is_empty():
		return failures
	var saved := {}
	for field in writable:
		saved[field] = initial["values"][field]
	for optional in ["vector2", "vector4", "rotation", "color", "enabled", "count", "weight"]:
		if initial["values"].has(optional):
			saved[optional] = initial["values"][optional]

	if initial["values"].has("enabled") and initial["values"].has("count") and initial["values"].has("weight"):
		var precise_number := 0.12345678901234566
		for exact_integer in [-9223372036854775807 - 1, 9223372036854775807, 0]:
			var flag: bool = exact_integer != 0
			var scalar_result := await _write(client, handle, {"enabled": flag, "count": exact_integer, "weight": precise_number})
			var scalar_values: Dictionary = client.get_entity_by_handle(handle).get("values", {})
			_check(scalar_result["status"] == 0, "scalar write failed", failures)
			_check(typeof(scalar_values.get("enabled")) == TYPE_BOOL and scalar_values.get("enabled") == flag, "bool became an integer", failures)
			_check(typeof(scalar_values.get("count")) == TYPE_INT and scalar_values.get("count") == exact_integer, "int64 lost type or precision", failures)
			_check(typeof(scalar_values.get("weight")) == TYPE_FLOAT and scalar_values.get("weight") == precise_number, "double lost type or precision", failures)

	var result := await _write(client, handle, {"position": Vector3(3.25, -2.5, 8.0)})
	_check(result["status"] == 0, "Vector3 write failed", failures)
	var values: Dictionary = client.get_entity_by_handle(handle).get("values", {})
	_check(values.get("position") == {"x": 3.25, "y": -2.5, "z": 8.0},
			"Vector3 members did not roundtrip", failures)

	var transform := Transform3D(Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 10)), Vector3(11, 12, 13))
	result = await _write(client, handle, {"matrix": transform})
	_check(result["status"] == 0, "Transform3D write failed", failures)
	values = client.get_entity_by_handle(handle).get("values", {})
	_check(values.get("matrix") == [[1.0, 4.0, 7.0, 11.0], [2.0, 5.0, 8.0, 12.0],
			[3.0, 6.0, 10.0, 13.0], [0.0, 0.0, 0.0, 1.0]], "Transform3D matrix is not row-major", failures)
	var projection := Projection(Vector4(1, 2, 3, 4), Vector4(5, 6, 7, 8), Vector4(9, 10, 11, 12), Vector4(13, 14, 15, 16))
	result = await _write(client, handle, {"matrix": projection})
	_check(result["status"] == 0, "Projection write failed", failures)
	values = client.get_entity_by_handle(handle).get("values", {})
	_check(values.get("matrix") == [[1.0, 5.0, 9.0, 13.0], [2.0, 6.0, 10.0, 14.0],
			[3.0, 7.0, 11.0, 15.0], [4.0, 8.0, 12.0, 16.0]], "Projection matrix is not row-major", failures)

	var entries := {"potion": 3, "daily.1": 7, "__fsnet_type": 9}
	var binary := PackedByteArray([0, 255, 128, 1, 65])
	var nested := {"label": "世界🌍", "points": [1.25, -2.0, 3.5]}
	result = await _write(client, handle, {"items": FsnetMap.from_dictionary(entries), "blob": binary, "nested": nested})
	_check(result["status"] == 0, "map/binary/nested write failed", failures)
	values = client.get_entity_by_handle(handle).get("values", {})
	var actual_map: Variant = values.get("items")
	_check(actual_map is FsnetMap, "native map lost its map wrapper", failures)
	if actual_map is FsnetMap:
		_check(actual_map.entries == entries, "map keys or values changed", failures)
	_check(values.get("blob") == binary, "binary string lost NUL/non-UTF-8 bytes", failures)
	_check(values.get("nested") == nested, "nested values or UTF-8 text changed", failures)

	if initial["values"].has("vector2"):
		result = await _write(client, handle, {"vector2": Vector2(0.25, -0.5)})
		values = client.get_entity_by_handle(handle).get("values", {})
		_check(result["status"] == 0 and values.get("vector2") == {"x": 0.25, "y": -0.5}, "Vector2 mapping failed", failures)
	if initial["values"].has("vector4"):
		result = await _write(client, handle, {"vector4": Vector4(0.125, -0.25, 0.5, 1.0)})
		values = client.get_entity_by_handle(handle).get("values", {})
		_check(result["status"] == 0 and values.get("vector4") == {"x": 0.125, "y": -0.25, "z": 0.5, "w": 1.0}, "Vector4 mapping failed", failures)
	if initial["values"].has("rotation"):
		for item in [Vector4(0.125, -0.25, 0.5, 1.0), Quaternion(0.125, -0.25, 0.5, 1.0)]:
			result = await _write(client, handle, {"rotation": item})
			values = client.get_entity_by_handle(handle).get("values", {})
			_check(result["status"] == 0 and values.get("rotation") == {"x": 0.125, "y": -0.25, "z": 0.5, "w": 1.0}, "Vector4/Quaternion mapping failed", failures)
	if initial["values"].has("color"):
		result = await _write(client, handle, {"color": Color(0.25, 0.5, 0.75, 1.0)})
		values = client.get_entity_by_handle(handle).get("values", {})
		_check(result["status"] == 0 and values.get("color") == {"r": 0.25, "g": 0.5, "b": 0.75, "a": 1.0}, "Color mapping failed", failures)

	await _rejected(client, handle, {1: 2}, "string keys", failures)
	await _rejected(client, handle, {"": 2}, "127", failures)
	await _rejected(client, handle, {"é".repeat(64): 2}, "127", failures)
	await _rejected(client, handle, {"position": {"x": NAN, "y": 0.0, "z": 0.0}}, "finite", failures)
	await _rejected(client, handle, {"position": Vector3(INF, 0, 0)}, "finite", failures)
	await _rejected(client, handle, {"blob": "x".repeat(65537)}, "65536", failures)
	await _rejected(client, handle, {"nested": RefCounted.new()}, "FsnetMap", failures)
	await _rejected(client, handle, {"items": FsnetMap.from_dictionary({1: 2})}, "string keys", failures)
	var too_deep: Variant = 0
	for _index in range(10):
		too_deep = {"child": too_deep}
	await _rejected(client, handle, {"nested": too_deep}, "depth/node", failures)
	var too_many := []
	too_many.resize(257)
	too_many.fill(0)
	await _rejected(client, handle, {"nested": too_many}, "256", failures)
	var row := []
	row.resize(256)
	row.fill(true)
	var aliases := []
	for _index in range(17):
		aliases.append(row)
	await _rejected(client, handle, {"nested": aliases}, "depth/node", failures)
	var repeated := "x".repeat(65536)
	var wide := []
	for _index in range(32):
		wide.append(repeated)
	await _rejected(client, handle, {"blob": wide}, "1 MiB", failures)
	var cycle := []
	cycle.append(cycle)
	await _rejected(client, handle, {"nested": cycle}, "depth/node", failures)
	cycle.clear()

	# A rejected conversion must not poison the client or consume its next write.
	result = await _write(client, handle, saved)
	_check(result["status"] == 0, "healthy write after invalid values failed", failures)
	return failures
