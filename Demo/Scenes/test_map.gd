extends Node3D

# Test map script: manages local and remote players for a simple 2-player test

@onready var player_scene := preload("res://Demo/Player/Player.tscn")

@onready var local_player: LocalPlayer = $Player
var remote_players := {}

var replica_update_timer := 0.0
const UPDATE_INTERVAL := 0.05

func _ready():
	# Listen for lifecycle events from the ReplicationService
	if not ReplicationService.player_joined.is_connected(_on_player_joined):
		ReplicationService.player_joined.connect(_on_player_joined)
	if not ReplicationService.player_left.is_connected(_on_player_left):
		ReplicationService.player_left.connect(_on_player_left)
	if not ReplicationService.player_updated.is_connected(_on_player_updated):
		ReplicationService.player_updated.connect(_on_player_updated)

func _process(delta):
	# Polling is handled by ReplicationService via native.poll in its _process
	# Send periodic local updates
	if local_player:
		replica_update_timer += delta
		if replica_update_timer >= UPDATE_INTERVAL:
			replica_update_timer = 0.0
			_send_local_state()

func _send_local_state():
	var self_uuid = ReplicationService.get_self_uuid()
	if self_uuid == "":
		return
	
	if not local_player:
		return
	
	var b64 := local_player.serialize()
	ReplicationService.send_actor_update(self_uuid, 0, 0, 0, b64)

# Remote player handlers
func _on_player_joined(data):
	# data contains uuid and initial state
	var uuid = data.uuid
	if remote_players.has(uuid): return
	var p = player_scene.instantiate()
	p.set_script(RemotePlayer)
	add_child(p)
	remote_players[uuid] = p
	# apply initial state
	_on_player_updated(data)

func _on_player_left(data):
	var uuid = data.uuid
	if not remote_players.has(uuid): return
	var p = remote_players[uuid]
	p.queue_free()
	remote_players.erase(uuid)

func _on_player_updated(data):
	var uuid = data.uuid
	if not remote_players.has(uuid): return
	var p = remote_players[uuid]
	if data.has("state") and data.state != "":
		p.deserialize(data.state)
		
