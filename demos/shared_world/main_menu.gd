extends Control


func _on_btn_play_online_pressed() -> void:
	get_tree().change_scene_to_file("res://demos/shared_world/login_menu.tscn")


func _on_btn_play_offline_pressed() -> void:
	get_tree().change_scene_to_file("res://demos/shared_world/world.tscn")


func _on_btn_exit_pressed() -> void:
	get_tree().quit()
