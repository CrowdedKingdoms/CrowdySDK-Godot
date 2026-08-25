extends Player

func _ready() -> void:
	camera.current = false
	
func apply_replication_state(
	new_position: Vector3,
	new_rotation: Vector3
) -> void:
	print("Remote player updated")
	print("Position: ", new_position)
	print("Rotation: ", new_rotation)
	global_position = new_position
	global_rotation = new_rotation

func _physics_process(delta: float) -> void:
	super._physics_process(delta)

	update_model_animation(delta)
