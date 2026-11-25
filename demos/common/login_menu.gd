class_name LoginMenu
extends Control
## Signs the player in (or registers them) and starts the Crowdy session of the demo picked in the
## main menu before loading it, so every refusal is shown here, where the player can act on it.

const SCENE_PATH := "res://demos/common/login_menu.tscn"

## Friendlier text for the refusals a player can act on; others show the server's message.
const MESSAGES := {
	"EMAIL_CONFIRMATION_REQUIRED": "Confirm your email first: open the link we sent you.",
	"LEGAL_ACCEPTANCE_REQUIRED": "Accept the documents above to play.",
	"AGE_REQUIRED": "This account needs a birth date before it can play.",
}

## The demo to sign in for, set by [method open] before the menu enters the tree.
var demo: DemoInfo

@onready var email_field: LineEdit = $VBoxContainer/InputFields/EmailField
@onready var password_field: LineEdit = $VBoxContainer/InputFields/PasswordField
@onready var status_label: Label = $VBoxContainer/Status
@onready var documents_label: RichTextLabel = $VBoxContainer/Documents
@onready var accept_documents: CheckBox = $VBoxContainer/AcceptDocuments


## Shows the login menu for [param picked_demo].
static func open(tree: SceneTree, picked_demo: DemoInfo) -> void:
	var menu: LoginMenu = load(SCENE_PATH).instantiate()
	menu.demo = picked_demo
	tree.change_scene_to_node(menu)


func _ready() -> void:
	if Crowdy.identity.is_signed_in():
		status_label.text = "Signed in. Press Login to play %s." % demo.title
	else:
		status_label.text = "Sign in to play %s." % demo.title
	_show_legal_documents()


func _on_btn_back_pressed() -> void:
	MainMenu.open(get_tree())


func _on_btn_login_pressed() -> void:
	# A session restored from the last launch plays without typing the password again.
	if not (Crowdy.identity.is_signed_in() and email_field.text.is_empty()):
		status_label.text = "Signing in..."
		var signed_in: CrowdyResult = await Crowdy.identity.sign_in(email_field.text, password_field.text).completed
		if not signed_in.ok:
			_show_failure("Sign-in failed", signed_in)
			return
	_enter_world()


func _on_btn_register_pressed() -> void:
	status_label.text = "Registering..."
	var registered: CrowdyResult = await Crowdy.identity.register_user(email_field.text, password_field.text).completed
	if not registered.ok:
		_show_failure("Registration failed", registered)
		return
	status_label.text = "Account created. Open the confirmation link we emailed you, then log in."


func _on_documents_meta_clicked(meta: Variant) -> void:
	OS.shell_open(str(meta))


func _enter_world() -> void:
	status_label.text = "Connecting..."
	if accept_documents.button_pressed:
		var accepted: CrowdyResult = await Crowdy.identity.accept_legal_documents(_app_id()).completed
		if not accepted.ok:
			_show_failure("Could not record your acceptance", accepted)
			return

	var started: CrowdyResult = await Crowdy.start_session(demo.session_config)
	if not started.ok:
		_show_failure("Could not join the world", started)
		return
	get_tree().change_scene_to_file(demo.world_scene)


func _show_legal_documents() -> void:
	var fetched: CrowdyResult = await Crowdy.identity.fetch_legal_documents(_app_id()).completed
	if not fetched.ok:
		documents_label.text = "Could not load the documents players accept."
		return
	var links := PackedStringArray()
	for document: Dictionary in fetched.data:
		links.append("[url=%s]%s[/url]" % [document.url, document.title])
	documents_label.text = "Player documents: " + ", ".join(links)


func _show_failure(action: String, result: CrowdyResult) -> void:
	status_label.text = "%s: %s" % [action, MESSAGES.get(result.error_code, result.error_message)]


func _app_id() -> String:
	return ProjectSettings.get_setting(Crowdy.SETTING_APP_ID, "")
