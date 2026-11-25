class_name ArenaLobby
extends Control
## Where Push Arena starts online: play the public arena, join a public match from the list,
## create a match, or join a private match by its chunk's coordinates. Leaving an arena ends its
## session (that is how the other players see the player go), so the lobby starts a new one.

const SCENE_PATH := "res://demos/push_arena/lobby/arena_lobby.tscn"
const SESSION_CONFIG := preload("res://demos/push_arena/session_config.tres")

var _player_name := "Player"
## Set while a lookup runs, so a second click waits for the first.
var _busy := false

@onready var status_label: Label = %StatusLabel
@onready var match_list: VBoxContainer = %MatchList
@onready var mode_option: OptionButton = %ModeOption
@onready var visibility_option: OptionButton = %VisibilityOption
@onready var coordinates_field: LineEdit = %CoordinatesField


static func open(tree: SceneTree) -> void:
	tree.change_scene_to_file(SCENE_PATH)


func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_VISIBLE
	_busy = true
	if Crowdy.session.get_state() == CrowdyGameSession.STATE_STOPPED:
		status_label.text = "Connecting..."
		var started: CrowdyResult = await Crowdy.start_session(SESSION_CONFIG)
		if not started.ok:
			status_label.text = "Could not connect: %s" % started.error_message
			return
	var profile: CrowdyResult = await Crowdy.identity.fetch_profile().completed
	if profile.ok and not str(profile.data.gamertag).is_empty():
		_player_name = profile.data.gamertag
	_busy = false
	_refresh_public_matches()


func _on_btn_public_arena_pressed() -> void:
	if not _busy:
		PushArena.open(get_tree(), MatchDirectory.PUBLIC_ARENA_CHUNK, null, _player_name)


func _on_btn_refresh_pressed() -> void:
	if not _busy:
		_refresh_public_matches()


func _on_btn_create_pressed() -> void:
	if _busy:
		return
	var config := MatchConfig.new()
	config.mode = mode_option.selected as MatchConfig.Mode
	config.visibility = visibility_option.selected as MatchConfig.Visibility
	config.creator_name = _player_name
	_busy = true
	status_label.text = "Finding a free chunk..."
	var found: CrowdyResult
	if config.visibility == MatchConfig.Visibility.PUBLIC:
		found = await MatchDirectory.find_free_public_slot()
	else:
		found = await MatchDirectory.find_free_private_chunk()
	_busy = false
	if not found.ok:
		status_label.text = "Could not create the match: %s" % found.error_message
		return
	config.chunk = found.data
	_open_match(config)


func _on_btn_join_pressed() -> void:
	if _busy:
		return
	var parts := coordinates_field.text.replace(",", " ").split(" ", false)
	if parts.size() != 2 or not parts[0].is_valid_int() or not parts[1].is_valid_int():
		status_label.text = "Type the match's coordinates as x, z."
		return
	var chunk := Vector3i(parts[0].to_int(), 0, parts[1].to_int())
	_busy = true
	status_label.text = "Looking for the match..."
	var read := await MatchDirectory.read_match(chunk)
	_busy = false
	if not read.ok:
		status_label.text = "Could not look for the match: %s" % read.error_message
	elif read.data == null:
		status_label.text = "There is no match at %d, %d." % [chunk.x, chunk.z]
	else:
		_open_match(read.data)


func _on_btn_back_pressed() -> void:
	MainMenu.open(get_tree())


func _open_match(config: MatchConfig) -> void:
	PushArena.open(get_tree(), config.chunk, config, _player_name)


func _refresh_public_matches() -> void:
	_busy = true
	status_label.text = "Looking for public matches..."
	var listed := await MatchDirectory.list_public_matches()
	_busy = false
	for row in match_list.get_children():
		row.queue_free()
	if not listed.ok:
		status_label.text = "Could not list the public matches: %s" % listed.error_message
		return
	status_label.text = "Signed in as %s." % _player_name
	if listed.data.is_empty():
		var none := Label.new()
		none.text = "No public matches right now. Create one!"
		match_list.add_child(none)
	for config: MatchConfig in listed.data:
		match_list.add_child(_create_match_row(config))


func _create_match_row(config: MatchConfig) -> HBoxContainer:
	var row := HBoxContainer.new()
	var description := Label.new()
	var mode := "Rounds" if config.mode == MatchConfig.Mode.ROUNDS else "Endless"
	description.text = "%s by %s, %d playing" % [mode, config.creator_name, config.player_count]
	description.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(description)
	var join := Button.new()
	join.text = "Join"
	join.pressed.connect(_open_match.bind(config))
	row.add_child(join)
	return row
