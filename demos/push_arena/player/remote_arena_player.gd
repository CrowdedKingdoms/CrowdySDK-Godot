class_name RemoteArenaPlayer
extends ArenaPlayer
## Another player in the arena, shown from the states the realtime session receives. States arrive
## 20 times a second, so the body eases toward the latest one instead of jumping to it.

## How quickly the body catches up with the latest received state.
const SMOOTHING := 15.0

var _target_position := Vector3.ZERO
var _target_yaw := 0.0
var _has_target := false


func _physics_process(delta: float) -> void:
	if not _has_target:
		return
	var weight := minf(1.0, SMOOTHING * delta)
	global_position = global_position.lerp(_target_position, weight)
	face(lerp_angle(facing_yaw, _target_yaw, weight))


func apply_state(state: PlayerState) -> void:
	var returning := is_out() and state.action != Action.OUT
	_target_position = state.position
	_target_yaw = state.yaw
	if not _has_target or returning:
		# First seen, or back through the portal: appear there instead of gliding in.
		_has_target = true
		global_position = _target_position
		face(_target_yaw)
	show_action(state.action)
	if player_name != state.player_name:
		player_name = state.player_name
		name_tag.text = player_name
