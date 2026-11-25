@tool
extends EditorPlugin
## Installs the SDK into a project: the Crowdy autoload and its project settings.

const AUTOLOAD_NAME := "Crowdy"
const AUTOLOAD_PATH := "res://addons/crowdy_sdk/crowdy.gd"
const CrowdyScript := preload("res://addons/crowdy_sdk/crowdy.gd")


func _enter_tree() -> void:
	# Property info and defaults are not saved with the project, so they are declared on every
	# editor start to show the settings in Project Settings.
	_declare_setting(CrowdyScript.SETTING_API_URL, "", TYPE_STRING)
	_declare_setting(CrowdyScript.SETTING_APP_ID, "", TYPE_STRING)
	_declare_setting(CrowdyScript.SETTING_PERSIST_SESSION, true, TYPE_BOOL)


func _enable_plugin() -> void:
	add_autoload_singleton(AUTOLOAD_NAME, AUTOLOAD_PATH)


func _disable_plugin() -> void:
	remove_autoload_singleton(AUTOLOAD_NAME)


func _declare_setting(setting_name: String, default_value: Variant, type: Variant.Type) -> void:
	if not ProjectSettings.has_setting(setting_name):
		ProjectSettings.set_setting(setting_name, default_value)
	ProjectSettings.set_initial_value(setting_name, default_value)
	ProjectSettings.add_property_info({"name": setting_name, "type": type})
