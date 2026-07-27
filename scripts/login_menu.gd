extends Control

@onready var email_field = $VBoxContainer/InputFields/EmailField
@onready var password_field = $VBoxContainer/InputFields/PasswordField
@onready var status_label = $VBoxContainer/Status

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	status_label.text = "Ready"


# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(_delta: float) -> void:
	pass

func _on_btn_back_pressed() -> void:
	get_tree().change_scene_to_file("res://scenes/main_menu.tscn")

func _on_btn_login_pressed() -> void:
	var email = email_field.text
	var password = password_field.text
	status_label.text = "Logging in..."
	var res = AuthService.login(email, password)

func _on_btn_register_pressed() -> void:
	var email = email_field.text
	var password = password_field.text
	status_label.text = "Registering..."
	# Placeholder: CrowdyCPP registration endpoint not exposed in this wrapper yet
	status_label.text = "Register not implemented"
