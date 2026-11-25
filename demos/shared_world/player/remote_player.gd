class_name RemotePlayer
extends Player
## Another player, driven by the states the realtime session receives. Updates arrive a few
## times a second, so the body eases toward the latest one instead of jumping to it.

## How quickly the body catches up with the latest received state.
const SMOOTHING := 15.0

var _target_position: Vector3
var _target_yaw: float
var _has_target := false


func apply_state(bytes: PackedByteArray) -> void:
	var decoded := Player.decode_state(bytes)
	if decoded.is_empty():
		push_warning("Ignoring a remote player state of %d bytes." % bytes.size())
		return
	_target_position = decoded.position
	_target_yaw = decoded.yaw
	if not _has_target:
		_has_target = true
		global_position = _target_position
		rotation.y = _target_yaw
	state = decoded.state
	update_model_animation(0.0)


func _physics_process(delta: float) -> void:
	if not _has_target:
		return
	var weight := minf(1.0, SMOOTHING * delta)
	global_position = global_position.lerp(_target_position, weight)
	rotation.y = lerp_angle(rotation.y, _target_yaw, weight)
