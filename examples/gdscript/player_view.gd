extends Node3D
## Rendering belongs to this example; the addon has no Player gameplay class.

var entity_id: String = ""
var controlled: bool = false
var owned: bool = false
var confirmed: Dictionary = {}
var _mesh: MeshInstance3D


func _ready() -> void:
	_mesh = MeshInstance3D.new()
	_mesh.mesh = CapsuleMesh.new()
	add_child(_mesh)
	_update_color()


func apply_entity(entity: Dictionary) -> void:
	entity_id = str(entity.get("id", ""))
	owned = entity.get("owned") == true
	controlled = bool(entity.get("controlled", false))
	confirmed = entity.get("values", {})
	var current_position: Variant = confirmed.get("position")
	if current_position is Vector3:
		position = current_position
	elif current_position is Dictionary:
		position = Vector3(float(current_position.get("x", 0.0)), float(current_position.get("y", 0.0)), float(current_position.get("z", 0.0)))
	_update_color()


func _update_color() -> void:
	if _mesh == null:
		return
	var material := StandardMaterial3D.new()
	material.albedo_color = Color(0.15, 0.55, 1.0) if owned else Color(1.0, 0.55, 0.15)
	_mesh.material_override = material
