class_name LocalPlayer
extends Player

@export var speed := 5.0
@export var mouse_sens := 0.002

var yaw := 0.0
var pitch := 0.0

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
	var input_dir := Input.get_vector(
		"move_left",
		"move_right",
		"move_forward",
		"move_back"
	)
	
	if input_dir.length() > 0:
		state = State.WALKING

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

	if not is_on_floor():
		velocity.y -= gravity * delta
		state = State.FALLING
	elif state == State.FALLING:
		state = State.LANDING
	elif direction.length() > 0.0:
		state = State.WALKING
	else:
		state = State.IDLE

	move_and_slide()
	update_model_animation(delta)
	
