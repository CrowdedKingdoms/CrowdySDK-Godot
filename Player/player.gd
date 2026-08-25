extends CharacterBody3D
class_name Player

@onready var model: Node3D = $Model
@onready var cam_pivot: SpringArm3D = $SpringArm3D
@onready var camera: Camera3D = $SpringArm3D/Camera3D

@onready var Anim: AnimationPlayer = $Model/AnimationPlayer
var anim_locked = false
var current_anim = ""

var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")

var moving: bool = false
var falling: bool = false

func _physics_process(delta: float) -> void:
	# Gravity is common to both players if you want it.
	if not is_on_floor():
		velocity.y -= gravity * delta
		falling = true
	else:
		if falling:
			_play_anim("Jump_Land", true)
			falling = false

func _play_anim(anim_name: String, locked: bool = false, interrupt: bool = false) -> void:
	if current_anim == anim_name:
		return
	
	if anim_locked and !interrupt:
		return
	
	anim_locked = locked
	current_anim = anim_name
	Anim.play(current_anim)

func _on_animation_player_animation_finished(anim_name: StringName) -> void:
	if anim_locked:
		anim_locked = false
		
func update_model_animation(delta: float) -> void:
	if falling:
		_play_anim("Jump", false, true)
	elif moving:
		_play_anim("Walk", false, true)
	else:
		_play_anim("Idle")
