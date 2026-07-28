extends Control


# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	pass # Replace with function body.


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(_delta: float) -> void:
	pass

func _on_btn_play_online_pressed() -> void:
	get_tree().change_scene_to_file("res://Scenes/login_menu.tscn")

func _on_btn_play_offline_pressed() -> void:
	get_tree().change_scene_to_file("res://Scenes/test_map.tscn")
	
func _on_btn_options_pressed() -> void:
	pass # Replace with function body.

func _on_btn_exit_pressed() -> void:
	get_tree().quit()
