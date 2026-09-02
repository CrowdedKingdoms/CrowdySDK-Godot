class_name Player
extends CharacterBody3D

@onready var model: Node3D = $Model
@onready var cam_pivot: SpringArm3D = $SpringArm3D
@onready var camera: Camera3D = $SpringArm3D/Camera3D
@onready var Anim: AnimationPlayer = $Model/AnimationPlayer

var anim_locked = false
var current_anim = ""

var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")

var state: State = State.IDLE

enum State {
	IDLE,
	WALKING,
	FALLING,
	LANDING
}

func serialize() -> String:
	var pos = global_transform.origin
	var rot = rotation
	
	var s = {"pos":[pos.x, pos.y, pos.z], 
			 "rot":[rot.x, rot.y, rot.z],
			 "state": state}

	var raw = JSON.stringify(s)
	var bytes = raw.to_utf8_buffer()
	var b64 = Marshalls.raw_to_base64(bytes)
	
	return b64

func deserialize(b64) -> void:
	var bytes = Marshalls.base64_to_raw(b64)
	var json_string = bytes.get_string_from_utf8()
	var decoded = JSON.parse_string(json_string)

	if decoded == null:
		push_error("Failed to deserialize player state")
		return

	global_position = Vector3(
		decoded["pos"][0],
		decoded["pos"][1],
		decoded["pos"][2]
	)

	global_rotation = Vector3(
		decoded["rot"][0],
		decoded["rot"][1],
		decoded["rot"][2]
	)
	
	state = int(decoded["state"]) as State
	update_model_animation(0)

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
