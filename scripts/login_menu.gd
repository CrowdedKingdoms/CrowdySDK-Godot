extends Control

@onready var email_field = $VBoxContainer/InputFields/EmailField
@onready var password_field = $VBoxContainer/InputFields/PasswordField
@onready var status_label = $VBoxContainer/Status

# Called when the node enters the scene tree for the first time.
func _ready() -> void:
	status_label.text = "Ready"
	# Connect to AuthService signals
	if not AuthService.login_completed.is_connected(_on_login_result):
		AuthService.login_completed.connect(_on_login_result)
	if not AuthService.register_completed.is_connected(_on_register_result):
		AuthService.register_completed.connect(_on_register_result)

# Called every frame. 'delta' is the elapsed time since the previous frame.
func _process(_delta: float) -> void:
	pass

func _on_btn_back_pressed() -> void:
	get_tree().change_scene_to_file("res://Scenes/main_menu.tscn")

func _on_btn_login_pressed() -> void:
	var email = email_field.text
	var password = password_field.text
	status_label.text = "Logging in..."
	AuthService.login_async(email, password)

func _on_btn_register_pressed() -> void:
	var email = email_field.text
	var password = password_field.text
	status_label.text = "Registering..."
	AuthService.register_async(email, password, "")

func _on_login_result(res) -> void:
	if typeof(res) == TYPE_DICTIONARY:
		if res.has("ok") and res.ok:
			status_label.text = "Logged in"
			get_tree().change_scene_to_file("res://Scenes/test_map.tscn")
			return
		else:
			status_label.text = "Login failed"
			return
	status_label.text = "Login failed: invalid response"

func _on_register_result(res) -> void:
	if typeof(res) == TYPE_DICTIONARY:
		if res.has("ok") and res.ok:
			status_label.text = "Registered and logged in"
			get_tree().change_scene_to_file("res://Scenes/main_menu.tscn")
			return
		else:
			status_label.text = "Register failed"
			return
	status_label.text = "Register failed: invalid response"
