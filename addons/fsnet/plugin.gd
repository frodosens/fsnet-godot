@tool
extends EditorPlugin

const SINGLETON_NAME := "Fsnet"
const SINGLETON_PATH := "res://addons/fsnet/fsnet.gd"


func _enable_plugin() -> void:
	if not ClassDB.class_exists("FsnetClient"):
		push_error("fsnet native extension is unavailable. Install the binary matching this platform and restart Godot.")
		return
	if ProjectSettings.has_setting("autoload/" + SINGLETON_NAME):
		var existing: String = str(ProjectSettings.get_setting("autoload/" + SINGLETON_NAME))
		if existing.trim_prefix("*") != SINGLETON_PATH:
			push_error("The Fsnet autoload name is already used by another script.")
		return
	add_autoload_singleton(SINGLETON_NAME, SINGLETON_PATH)


func _disable_plugin() -> void:
	if ProjectSettings.has_setting("autoload/" + SINGLETON_NAME):
		var existing: String = str(ProjectSettings.get_setting("autoload/" + SINGLETON_NAME))
		if existing.trim_prefix("*") == SINGLETON_PATH:
			remove_autoload_singleton(SINGLETON_NAME)
