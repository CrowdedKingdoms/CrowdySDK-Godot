extends Node

# Project Settings keys
const GAME_API_URL_SETTING := "game/game_api_url"
const APP_ID_SETTING := "game/app_id"


# Read-only configuration properties

var game_api_url: String:
	get:
		return ProjectSettings.get_setting(GAME_API_URL_SETTING, "")


var app_id: String:
	get:
		return ProjectSettings.get_setting(APP_ID_SETTING, "")


# Optional validation helper
func is_valid() -> bool:
	return not game_api_url.is_empty() and not app_id.is_empty()


func print_config() -> void:
	print("Game API URL: ", game_api_url)
	print("App ID: ", app_id)
	
func _ready() -> void:
	print_config()
