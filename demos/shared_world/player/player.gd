class_name Player
extends CharacterBody3D

enum State {
	IDLE,
	WALKING,
	FALLING,
	LANDING
}

## Replicated state layout: position (3 floats), yaw (float), animation state (byte).
const STATE_SIZE := 17

@onready var model: Node3D = $Model
@onready var cam_pivot: SpringArm3D = $SpringArm3D
@onready var camera: Camera3D = $SpringArm3D/Camera3D
@onready var Anim: AnimationPlayer = $Model/AnimationPlayer

var anim_locked = false
var current_anim = ""

var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")

var state: State = State.IDLE


## What remote copies of this player need, packed for the realtime session.
func encode_state() -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(STATE_SIZE)
	bytes.encode_float(0, global_position.x)
	bytes.encode_float(4, global_position.y)
	bytes.encode_float(8, global_position.z)
	bytes.encode_float(12, global_rotation.y)
	bytes.encode_u8(16, state)
	return bytes


## Unpacks what [method encode_state] produced: [code]position[/code], [code]yaw[/code] and
## [code]state[/code]. Empty when [param bytes] is not a player state.
static func decode_state(bytes: PackedByteArray) -> Dictionary:
	if bytes.size() != STATE_SIZE:
		return {}
	return {
		"position": Vector3(bytes.decode_float(0), bytes.decode_float(4), bytes.decode_float(8)),
		"yaw": bytes.decode_float(12),
		"state": bytes.decode_u8(16) as State,
	}


func _play_anim(anim_name: String, locked: bool = false, interrupt: bool = false) -> void:
	if current_anim == anim_name:
		return

	if anim_locked and !interrupt:
		return

	anim_locked = locked
	current_anim = anim_name
	Anim.play(current_anim)

func _on_animation_player_animation_finished(_anim_name: StringName) -> void:
	if anim_locked:
		anim_locked = false

func update_model_animation(_delta: float) -> void:
	match state:
		State.FALLING:
			_play_anim("Jump", false, true)
		State.WALKING:
			_play_anim("Walk", false, true)
		State.LANDING:
			_play_anim("Jump_Land", false, true)
		State.IDLE:
			_play_anim("Idle")
