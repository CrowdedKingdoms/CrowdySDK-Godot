extends CharacterBody3D

@export var speed := 5.0
@export var mouse_sens := 0.002

@onready var model: Node3D = $Model
@onready var cam_pivot: SpringArm3D = $SpringArm3D

@onready var Anim: AnimationPlayer = $Model/AnimationPlayer
var anim_locked = false
var current_anim = ""

var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")

var yaw := 0.0
var pitch := 0.0

var moving: bool = false
var falling: bool = false

func _ready() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	yaw = rotation.y


func _input(event) -> void:
	if event is InputEventMouseMotion:
		yaw -= event.relative.x * mouse_sens
		rotation.y = yaw

		pitch -= event.relative.y * mouse_sens
		pitch = clamp(pitch, deg_to_rad(-60), deg_to_rad(35))
		
		cam_pivot.rotation.x = pitch

	elif event is InputEventKey and event.pressed:
		if event.keycode == KEY_ESCAPE:
			get_tree().quit()


func _physics_process(delta: float) -> void:
	if not is_on_floor():
		velocity.y -= gravity * delta
		falling = true
	else:
		if falling:
			_play_anim("Jump_Land", true)
			falling = false

	var input_dir := Input.get_vector(
		"move_left",
		"move_right",
		"move_forward",
		"move_back"
	)
	
	moving = input_dir.length() > 0

	var forward := Vector3(
		sin(yaw),
		0,
		cos(yaw)
	)

	var right := Vector3(
		forward.z,
		0,
		-forward.x
	)

	var direction := (right * input_dir.x + forward * input_dir.y).normalized()

	if direction.length() > 0.0:
		velocity.x = direction.x * speed
		velocity.z = direction.z * speed
	else:
		velocity.x = move_toward(velocity.x, 0.0, speed)
		velocity.z = move_toward(velocity.z, 0.0, speed)

	var target_rot = cam_pivot.rotation.y

	model.rotation.y = lerp_angle(
		model.rotation.y,
		target_rot + PI,
		12.0 * delta
	)

	if falling:
		_play_anim("Jump", false, true)
	elif moving:
		_play_anim("Walk", false, true)
	else:
		_play_anim("Idle")

	move_and_slide()
	
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
