extends Node3D
## The push arena: the local player, one RemoteArenaPlayer per arena actor of the Crowdy session,
## the platform and the referee. It is the only arena script that talks to Crowdy: it sends the
## local player's state and the arena's events, and routes what arrives. Offline (no session
## started), the local player plays alone and referees the arena themselves.

## The Crowdy event types the arena sends. Any type from 0 to 65535 works; these are its own.
enum Event { PUSH = 1, FELL_OFF = 2, ARENA_STATE = 3 }

## Every arena player reports this one Crowdy chunk, 128 chunks (2 km) from the shared_world
## demo's players: both demos play the same Crowdy app, and this keeps them out of each other's
## range.
const ARENA_CHUNK := Vector3i(128, 0, 0)
## A PUSH event carries where the pusher stood (3 floats) and where they faced (a float).
const PUSH_PAYLOAD_SIZE := 16
## The host resends the arena state this often, for players who just arrived or lost a packet.
const STATE_RESEND_INTERVAL := 1.0
const REMOTE_PLAYER_SCENE := preload("res://demos/push_arena/player/remote_arena_player.tscn")

## Actor id -> RemoteArenaPlayer, for every arena player in range.
var _remote_players: Dictionary[String, RemoteArenaPlayer] = {}
var _stats := PlayerStats.new()
## Whether a Crowdy session was started for this visit; offline otherwise.
var _online := false
var _resend_time := 0.0

@onready var local_player: LocalArenaPlayer = $LocalPlayer
@onready var remote_players: Node3D = $RemotePlayers
@onready var platform: ArenaPlatform = $Platform
@onready var portal: Node3D = $Portal
@onready var referee: Referee = $Referee
@onready var hud: ArenaHud = $Hud


func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
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
	session.send_rejected.connect(_on_send_rejected)
	if not _online:
		return
	local_player.paint(ArenaPlayer.color_for(session.get_actor_id()))
	_load_player_name()
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
	hud.show_arena(referee.state, alive_ids.size(), player_count, _is_referee())
	hud.show_banner(_banner_text(alone))

	if Crowdy.session.has_joined():
		Crowdy.session.set_actor_state(local_player.get_state().encode())


func _unhandled_input(event: InputEvent) -> void:
	if event.is_action_pressed(&"ui_cancel"):
		MainMenu.open(get_tree())
	elif event.is_action_pressed(&"switch_mode"):
		if _is_referee():
			referee.switch_mode()
		else:
			hud.add_feed_line("Only the host picks the mode")
	elif Crowdy.session.get_state() == CrowdyGameSession.STATE_INACTIVE:
		# The server releases a session after the app's idle timeout; the player's next input
		# brings it back.
		Crowdy.session.reconnect()


## Whether this client decides the arena's rules: the host decides for everyone online.
func _is_referee() -> bool:
	return not _online or Crowdy.session.is_host()


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
	if alone and state.mode == ArenaState.Mode.ROUNDS:
		return "Last one standing: hold on!"
	return ""


## Shows the account's gamertag to the other players.
func _load_player_name() -> void:
	var profile: CrowdyResult = await Crowdy.identity.fetch_profile().completed
	if profile.ok and not str(profile.data.get("gamertag", "")).is_empty():
		local_player.player_name = profile.data.gamertag


func _join() -> void:
	Crowdy.session.join(ARENA_CHUNK, local_player.get_state().encode())


func _send_arena_state() -> void:
	_resend_time = 0.0
	if Crowdy.session.has_joined():
		Crowdy.session.send_event(Event.ARENA_STATE, referee.state.encode())


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
	if state.mode != previous.mode:
		hud.add_feed_line("Mode: %s" % ("Endless" if state.mode == ArenaState.Mode.ENDLESS else "Rounds"))
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


func _on_actor_joined(actor_id: String, _chunk: Vector3i, state_bytes: PackedByteArray) -> void:
	var state := PlayerState.decode(state_bytes)
	if state == null or _remote_players.has(actor_id):
		return
	var remote_player: RemoteArenaPlayer = REMOTE_PLAYER_SCENE.instantiate()
	remote_players.add_child(remote_player)
	remote_player.paint(ArenaPlayer.color_for(actor_id))
	remote_player.apply_state(state)
	_remote_players[actor_id] = remote_player
	hud.add_feed_line("%s joined" % remote_player.player_name)


func _on_actor_updated(actor_id: String, _chunk: Vector3i, state_bytes: PackedByteArray, _server_time_ms: int) -> void:
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


func _on_send_rejected(error_code: String, send_kind: String) -> void:
	push_warning("Crowdy refused a %s: %s" % [send_kind, error_code])
