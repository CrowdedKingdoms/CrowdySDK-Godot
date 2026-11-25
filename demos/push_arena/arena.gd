class_name PushArena
extends Node3D
## The push arena: the local player, one RemoteArenaPlayer per arena actor of the Crowdy session,
## the platform and the referee. During play it is the script that talks to Crowdy: it sends the
## local player's state, the arena's events and the match's voxel edits, and routes what arrives.
## Online it plays one Crowdy chunk, the public arena's or a match's (the lobby picks it).
## Offline (no session started), the local player plays the public arena alone and referees it.

## The Crowdy event types the arena sends. Any type from 0 to 65535 works; these are its own.
enum Event { PUSH = 1, FELL_OFF = 2, ARENA_STATE = 3 }

const SCENE_PATH := "res://demos/push_arena/arena.tscn"
## A PUSH event carries where the pusher stood (3 floats) and where they faced (a float).
const PUSH_PAYLOAD_SIZE := 16
## The referee resends the arena state this often, for players who just arrived or lost a packet.
const STATE_RESEND_INTERVAL := 1.0
## How often the local player's stats are checked against the records, and beaten ones written.
const RECORD_SAVE_INTERVAL := 5.0
const REMOTE_PLAYER_SCENE := preload("res://demos/push_arena/player/remote_arena_player.tscn")

## The Crowdy chunk this arena is: the public arena's, or a match's.
var _chunk := MatchDirectory.PUBLIC_ARENA_CHUNK
## The match being played, as its chunk stores it; null in the public arena and offline.
var _config: MatchConfig
var _player_name := "Player"
## Actor id -> RemoteArenaPlayer, for every arena player in range.
var _remote_players: Dictionary[String, RemoteArenaPlayer] = {}
var _stats := PlayerStats.new()
var _records := ArenaRecords.new()
## Whether the records were read from the chunk. None are written before, so a record not read
## yet is never overwritten with a lower one.
var _records_loaded := false
## Whether this player is creating the match. A new match starts with no records: its first
## config write also clears the ones an earlier match in the same chunk left.
var _new_match := false
## Whether a Crowdy session was started for this visit; offline otherwise.
var _online := false
var _resend_time := 0.0
## Starts due, so a referee writes the match's config as soon as it has joined.
var _heartbeat_time := MatchConfig.HEARTBEAT_INTERVAL
var _record_save_time := 0.0

@onready var local_player: LocalArenaPlayer = $LocalPlayer
@onready var remote_players: Node3D = $RemotePlayers
@onready var platform: ArenaPlatform = $Platform
@onready var portal: Node3D = $Portal
@onready var referee: Referee = $Referee
@onready var hud: ArenaHud = $Hud


## Opens the arena online in [param chunk]: the match [param config] describes, or the public
## arena when it is null. [param player_name] is shown to the other players.
static func open(tree: SceneTree, chunk: Vector3i, config: MatchConfig, player_name: String) -> void:
	var arena: PushArena = load(SCENE_PATH).instantiate()
	arena._chunk = chunk
	arena._config = config
	arena._player_name = player_name
	tree.change_scene_to_node(arena)


func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	referee.mode = _config.mode if _config else MatchConfig.Mode.ENDLESS
	local_player.player_name = _player_name
	local_player.spawn_position = portal.global_position
	local_player.pushed.connect(_on_local_player_pushed)
	local_player.fell_off.connect(_on_local_player_fell_off)
	referee.state_changed.connect(_on_arena_state_changed)
	platform.resize(referee.state.platform_radius)
	local_player.respawn()

	var session := Crowdy.session
	_online = session.get_state() != CrowdyGameSession.STATE_STOPPED
	session.state_changed.connect(_on_session_state_changed)
	session.actor_joined.connect(_on_actor_joined)
	session.actor_updated.connect(_on_actor_updated)
	session.actor_left.connect(_on_actor_left)
	session.event_received.connect(_on_event_received)
	session.voxel_updated.connect(_on_voxel_updated)
	session.send_rejected.connect(_on_send_rejected)
	if not _online:
		return
	local_player.paint(ArenaPlayer.color_for(session.get_actor_id()))
	_new_match = _config != null and _config.written_at_ms == 0
	if _new_match:
		_records_loaded = true
		hud.show_records(_records)
	else:
		_load_records()
	if session.get_state() == CrowdyGameSession.STATE_CONNECTED:
		_join()
	# Actors the session met before this scene was loaded.
	for actor: Dictionary in session.get_remote_actors():
		_on_actor_joined(actor.actor_id, actor.chunk, actor.state)


func _physics_process(delta: float) -> void:
	var alive_ids := _alive_ids()
	var player_count := 1 + _remote_players.size()
	if _is_referee():
		referee.decide(delta, alive_ids, player_count)
		_resend_time += delta
		if _resend_time >= STATE_RESEND_INTERVAL:
			_send_arena_state()
	_respawn_when_due()

	var on_platform := not local_player.is_out()
	var alone := on_platform and alive_ids.size() == 1 and player_count >= 2
	_stats.track(delta, on_platform, alone)
	hud.show_stats(_stats)
	hud.show_arena(referee.mode, referee.state, alive_ids.size(), player_count, _location_text())
	hud.show_banner(_banner_text(alone))

	if Crowdy.session.has_joined():
		Crowdy.session.set_actor_state(local_player.get_state().encode())
		_keep_match_alive(delta)
		_save_beaten_records(delta)


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed(&"ui_cancel"):
		if _online:
			# Ending the session is how a player leaves a chunk: the others see them go at once.
			Crowdy.session.stop()
			ArenaLobby.open(get_tree())
		else:
			MainMenu.open(get_tree())
	elif Crowdy.session.get_state() == CrowdyGameSession.STATE_INACTIVE:
		# The server releases a session after the app's idle timeout; the player's next input
		# brings it back.
		Crowdy.session.reconnect()


## Whether this client decides the arena's rules. Every client picks the same referee without
## asking anyone: the player in the arena with the lowest actor id.
func _is_referee() -> bool:
	if not _online:
		return true
	var local_id := Crowdy.session.get_actor_id()
	for actor_id in _remote_players:
		if actor_id < local_id:
			return false
	return true


## The actor ids of the players on the platform. Offline, the local player's id is empty.
func _alive_ids() -> PackedStringArray:
	var ids := PackedStringArray()
	if not local_player.is_out():
		ids.append(Crowdy.session.get_actor_id())
	for actor_id in _remote_players:
		if not _remote_players[actor_id].is_out():
			ids.append(actor_id)
	return ids


func _name_of(actor_id: String) -> String:
	if actor_id == Crowdy.session.get_actor_id():
		return local_player.player_name
	if _remote_players.has(actor_id):
		return _remote_players[actor_id].player_name
	return "Someone"


func _location_text() -> String:
	if not _online:
		return "Offline"
	if _config == null:
		return "Public arena"
	if _config.visibility == MatchConfig.Visibility.PUBLIC:
		return "Public match by %s" % _config.creator_name
	return "Private match at %d, %d" % [_chunk.x, _chunk.z]


## Brings the local player back once they have waited out the respawn delay, unless a round is
## being celebrated: then they come back with the next round.
func _respawn_when_due() -> void:
	var waited := local_player.out_time >= Referee.RESPAWN_DELAY
	var round_running := referee.state.phase == ArenaState.Phase.PLAYING
	if local_player.is_out() and waited and round_running:
		local_player.respawn()


func _banner_text(alone: bool) -> String:
	var state := referee.state
	if state.phase == ArenaState.Phase.ROUND_OVER:
		if local_player.cheering:
			return "You win round %d!" % state.round_number
		return "%s wins round %d" % [_name_of(state.winner_id), state.round_number]
	if local_player.is_out():
		return "Back in %d" % ceili(Referee.RESPAWN_DELAY - local_player.out_time)
	if alone and referee.mode == MatchConfig.Mode.ROUNDS:
		return "Last one standing: hold on!"
	return ""


func _join() -> void:
	Crowdy.session.join(_chunk, local_player.get_state().encode())


func _send_arena_state() -> void:
	_resend_time = 0.0
	if Crowdy.session.has_joined():
		Crowdy.session.send_event(Event.ARENA_STATE, referee.state.encode())


## The referee writes the match's config again every heartbeat interval, so the lobby lists the
## match as live, with its player count. The public arena has no config.
func _keep_match_alive(delta: float) -> void:
	_heartbeat_time += delta
	if _config == null or _heartbeat_time < MatchConfig.HEARTBEAT_INTERVAL or not _is_referee():
		return
	_heartbeat_time = 0.0
	_config.player_count = 1 + _remote_players.size()
	Crowdy.session.send_voxel_update(_chunk, MatchConfig.VOXEL, MatchConfig.VOXEL_TYPE, _config.encode())
	if _new_match:
		_new_match = false
		for stat: ArenaRecords.Stat in ArenaRecords.Stat.values():
			Crowdy.session.send_voxel_update(_chunk, ArenaRecords.voxel_of(stat), ArenaRecords.VOXEL_TYPE, _records.set_record(stat, 0.0, ""))


## Writes the records the local player has beaten, at most every RECORD_SAVE_INTERVAL.
func _save_beaten_records(delta: float) -> void:
	_record_save_time += delta
	if not _records_loaded or _record_save_time < RECORD_SAVE_INTERVAL:
		return
	_record_save_time = 0.0
	var beaten := _records.beaten_by(_stats)
	for stat in beaten:
		if _records.holders[stat] != local_player.player_name:
			hud.add_feed_line("You took the %s record" % ArenaRecords.NAMES[stat].to_lower())
		var state := _records.set_record(stat, ArenaRecords.value_in(_stats, stat), local_player.player_name)
		Crowdy.session.send_voxel_update(_chunk, ArenaRecords.voxel_of(stat), ArenaRecords.VOXEL_TYPE, state)
	if not beaten.is_empty():
		hud.show_records(_records)


## Reads the arena's records from its chunk. Records set later arrive through voxel_updated.
func _load_records() -> void:
	var fetched: CrowdyResult = await Crowdy.session.fetch_voxels(_chunk, _chunk).completed
	if not fetched.ok:
		hud.add_feed_line("Could not read the records: %s" % fetched.error_message)
		return
	for entry: Dictionary in fetched.data:
		var stat := ArenaRecords.stat_of(entry.voxel, entry.voxel_type)
		if stat != -1:
			_records.read(stat, entry.state)
	_records_loaded = true
	hud.show_records(_records)


## Adds a fall to the feed, and credits the pusher when that is the local player.
func _report_fall(faller_id: String, pusher_id: String) -> void:
	if pusher_id.is_empty():
		hud.add_feed_line("%s fell off" % _name_of(faller_id))
		return
	hud.add_feed_line("%s pushed %s off" % [_name_of(pusher_id), _name_of(faller_id)])
	if pusher_id == Crowdy.session.get_actor_id():
		_stats.players_pushed_off += 1


func _on_local_player_pushed(from: Vector3, yaw: float) -> void:
	if not Crowdy.session.has_joined():
		return
	var payload := PackedByteArray()
	payload.resize(PUSH_PAYLOAD_SIZE)
	payload.encode_float(0, from.x)
	payload.encode_float(4, from.y)
	payload.encode_float(8, from.z)
	payload.encode_float(12, yaw)
	Crowdy.session.send_event(Event.PUSH, payload)


func _on_local_player_fell_off(pusher_id: String) -> void:
	_stats.record_fall()
	_report_fall(Crowdy.session.get_actor_id(), pusher_id)
	if Crowdy.session.has_joined():
		Crowdy.session.send_event(Event.FELL_OFF, pusher_id.to_ascii_buffer())


func _on_arena_state_changed(previous: ArenaState) -> void:
	var state := referee.state
	platform.resize(state.platform_radius)
	if state.round_number != previous.round_number and local_player.is_out():
		# A new round starts with everyone on the platform.
		local_player.respawn()
	var round_won := state.phase == ArenaState.Phase.ROUND_OVER
	local_player.cheering = round_won and not local_player.is_out() and state.winner_id == Crowdy.session.get_actor_id()
	if round_won and previous.phase != ArenaState.Phase.ROUND_OVER:
		hud.add_feed_line("%s won round %d" % [_name_of(state.winner_id), state.round_number])
		if local_player.cheering:
			_stats.rounds_won += 1
	if _is_referee():
		_send_arena_state()


func _on_session_state_changed(state: CrowdyGameSession.State) -> void:
	if state == CrowdyGameSession.STATE_CONNECTED and not Crowdy.session.has_joined():
		_join()


func _on_actor_joined(actor_id: String, _chunk_of_actor: Vector3i, state_bytes: PackedByteArray) -> void:
	var state := PlayerState.decode(state_bytes)
	if state == null or _remote_players.has(actor_id):
		return
	var remote_player: RemoteArenaPlayer = REMOTE_PLAYER_SCENE.instantiate()
	remote_players.add_child(remote_player)
	remote_player.paint(ArenaPlayer.color_for(actor_id))
	remote_player.apply_state(state)
	_remote_players[actor_id] = remote_player
	hud.add_feed_line("%s joined" % remote_player.player_name)


func _on_actor_updated(actor_id: String, _chunk_of_actor: Vector3i, state_bytes: PackedByteArray, _server_time_ms: int) -> void:
	var remote_player: RemoteArenaPlayer = _remote_players.get(actor_id)
	var state := PlayerState.decode(state_bytes)
	if remote_player and state:
		remote_player.apply_state(state)


func _on_actor_left(actor_id: String) -> void:
	var remote_player: RemoteArenaPlayer = _remote_players.get(actor_id)
	if remote_player:
		hud.add_feed_line("%s left" % remote_player.player_name)
		remote_player.queue_free()
		_remote_players.erase(actor_id)


func _on_event_received(event_type: int, sender_id: String, payload: PackedByteArray, _from_server: bool) -> void:
	match event_type:
		Event.PUSH:
			if payload.size() == PUSH_PAYLOAD_SIZE:
				var from := Vector3(payload.decode_float(0), payload.decode_float(4), payload.decode_float(8))
				local_player.take_push(sender_id, from, payload.decode_float(12))
		Event.FELL_OFF:
			_report_fall(sender_id, payload.get_string_from_ascii())
		Event.ARENA_STATE:
			var state := ArenaState.decode(payload)
			if state and not _is_referee():
				referee.adopt(state)


## Records another player set. The local player's own edits come back too, already applied.
func _on_voxel_updated(chunk: Vector3i, voxel: Vector3i, voxel_type: int, state: PackedByteArray, sender_id: String) -> void:
	var stat := ArenaRecords.stat_of(voxel, voxel_type)
	if chunk != _chunk or stat == -1 or sender_id == Crowdy.session.get_actor_id():
		return
	var previous_holder := _records.holders[stat]
	if _records.read(stat, state) and _records.holders[stat] != previous_holder:
		hud.add_feed_line("%s took the %s record" % [_records.holders[stat], ArenaRecords.NAMES[stat].to_lower()])
	hud.show_records(_records)


func _on_send_rejected(error_code: String, send_kind: String) -> void:
	hud.add_feed_line("Crowdy refused a %s: %s" % [send_kind, error_code])
