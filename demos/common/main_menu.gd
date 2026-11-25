class_name MainMenu
extends Control
## Lists the demos; each plays online (after the login menu) or offline. Each demo starts its own
## Crowdy session, so returning here ends the session of the demo the player left.

const SCENE_PATH := "res://demos/common/main_menu.tscn"

## The demos listed, in order.
@export var demos: Array[DemoInfo] = []

@onready var demo_list: VBoxContainer = %DemoList


## Leaves the current demo for the main menu.
static func open(tree: SceneTree) -> void:
	tree.change_scene_to_file(SCENE_PATH)


func _ready() -> void:
	Crowdy.session.stop()
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	for demo in demos:
		demo_list.add_child(_create_row(demo))


func _create_row(demo: DemoInfo) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override(&"separation", 15)
	var title := Label.new()
	title.text = demo.title
	title.theme_type_variation = &"LargeLabel"
	title.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(title)
	row.add_child(_create_button("Online", func() -> void: LoginMenu.open(get_tree(), demo)))
	row.add_child(_create_button("Offline", func() -> void: get_tree().change_scene_to_file(demo.world_scene)))
	return row


func _create_button(text: String, on_pressed: Callable) -> Button:
	var button := Button.new()
	button.text = text
	button.theme_type_variation = &"LargeButton"
	button.custom_minimum_size.x = 160
	button.pressed.connect(on_pressed)
	return button


func _on_btn_exit_pressed() -> void:
	get_tree().quit()
