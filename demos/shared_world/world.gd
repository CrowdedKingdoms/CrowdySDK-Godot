extends Node3D
## The world: the local player, plus one RemotePlayer per actor of the Crowdy session.
## Offline (no session started) it is just the local player.

const REMOTE_PLAYER_SCENE := preload("res://demos/shared_world/player/remote_player.tscn")
const SESSION_CONFIG := preload("res://demos/shared_world/session_config.tres")

@onready var local_player: LocalPlayer = $Player

## Actor id -> RemotePlayer.
var _remote_players := {}


func _ready() -> void:
	var session := Crowdy.session
	session.state_changed.connect(_on_session_state_changed)
	session.actor_joined.connect(_on_actor_joined)
	session.actor_updated.connect(_on_actor_updated)
	session.actor_left.connect(_on_actor_left)

	if session.get_state() == CrowdyGameSession.STATE_CONNECTED:
		_join()
	# Actors the session met before this scene was loaded.
	for actor in session.get_remote_actors():
		_on_actor_joined(actor.actor_id, actor.chunk, actor.state)


func _physics_process(_delta: float) -> void:
	var session := Crowdy.session
	if not session.has_joined():
		return
	session.set_actor_state(local_player.encode_state())
	session.move_to_chunk(SESSION_CONFIG.world_to_chunk(local_player.global_position))


func _unhandled_input(_event: InputEvent) -> void:
	# The server releases a session after the app's idle timeout; the player's next input
	# brings it back.
	if Crowdy.session.get_state() == CrowdyGameSession.STATE_INACTIVE:
		Crowdy.session.reconnect()


func _join() -> void:
	Crowdy.session.join(SESSION_CONFIG.world_to_chunk(local_player.global_position), local_player.encode_state())


func _on_session_state_changed(state: CrowdyGameSession.State) -> void:
	if state == CrowdyGameSession.STATE_CONNECTED and not Crowdy.session.has_joined():
		_join()


func _on_actor_joined(actor_id: String, _chunk: Vector3i, state: PackedByteArray) -> void:
	if _remote_players.has(actor_id):
		return
	var remote_player: RemotePlayer = REMOTE_PLAYER_SCENE.instantiate()
	add_child(remote_player)
	_remote_players[actor_id] = remote_player
	remote_player.apply_state(state)


func _on_actor_updated(actor_id: String, _chunk: Vector3i, state: PackedByteArray, _server_time_ms: int) -> void:
	var remote_player: RemotePlayer = _remote_players.get(actor_id)
	if remote_player:
		remote_player.apply_state(state)


func _on_actor_left(actor_id: String) -> void:
	var remote_player: RemotePlayer = _remote_players.get(actor_id)
	if remote_player:
		remote_player.queue_free()
		_remote_players.erase(actor_id)
