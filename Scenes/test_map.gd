extends Node3D

# Test map script: manages local and remote players for a simple 2-player test

@onready var player_scene := preload("res://Player/Player.tscn")

@onready var local_player: Node3D = $Player
var remote_players := {}

var replica_update_timer := 0.0
const UPDATE_INTERVAL := 0.1 # 10 updates per second

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
	# Extract position and rotation and send via replication
	var pos = local_player.global_transform.origin
	var rot = local_player.rotation
	# Pack a tiny JSON payload as base64 for the SDK
	var s = {"pos":[pos.x, pos.y, pos.z], "rot":[rot.x, rot.y, rot.z]}
	var raw = JSON.stringify(s)
	var bytes = raw.to_utf8_buffer()
	var b64 = Marshalls.raw_to_base64(bytes)
	var resp = ReplicationService.send_actor_update(self_uuid, 0, 0, 0, b64)

# Remote player handlers
func _on_player_joined(data):
	# data contains uuid and initial state
	var uuid = data.uuid
	if remote_players.has(uuid): return
	var p = player_scene.instantiate()
	p.set_script(load("res://Player/remote_player.gd"))
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
		var bytes = Marshalls.base64_to_raw(data.state)
		var json_string = bytes.get_string_from_utf8()
		var decoded = JSON.parse_string(json_string)

		var pos = Vector3(
			decoded["pos"][0],
			decoded["pos"][1],
			decoded["pos"][2]
		)

		var rot = Vector3(
			decoded["rot"][0],
			decoded["rot"][1],
			decoded["rot"][2]
		)
		
		p.apply_replication_state(pos, rot)
		
