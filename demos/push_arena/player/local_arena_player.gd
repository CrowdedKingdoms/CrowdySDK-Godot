class_name LocalArenaPlayer
extends ArenaPlayer
## The player at this keyboard: runs, jumps, pushes, rolls and dodges. This client owns the body:
## pushes from other players are applied here, and this client decides when the player has fallen
## off the platform.

## The player shoved from [param from], facing [param yaw]. Every other player checks whether the
## push reached them.
signal pushed(from: Vector3, yaw: float)
## The player fell off the platform. [param pusher_id] is the actor who pushed them shortly
## before, or empty when nobody did.
signal fell_off(pusher_id: String)

const MOVE_SPEED := 5.0
## Acceleration on the ground, in m/s². In the air the player steers with AIR_CONTROL of it.
const ACCELERATION := 40.0
const AIR_CONTROL := 0.3
const JUMP_VELOCITY := 5.5
const MOUSE_SENSITIVITY := 0.002
## How far the camera looks down and up, in radians.
const MIN_PITCH := -1.2
const MAX_PITCH := 0.5

## A push holds the player for PUSH_DURATION and can repeat after PUSH_COOLDOWN.
const PUSH_DURATION := 0.4
const PUSH_COOLDOWN := 0.7
## A push reaches players within PUSH_RANGE metres, at most PUSH_CONE_DEGREES either side of where
## the pusher faces, and at most PUSH_HEIGHT above or below them.
const PUSH_RANGE := 1.8
const PUSH_CONE_DEGREES := 55.0
const PUSH_HEIGHT := 1.5
## The velocity a push gives the player it reaches: forward from the pusher, and up.
const KNOCKBACK_SPEED := 6.5
const KNOCKBACK_LIFT := 2.5
## A pushed player has no control for STAGGER_DURATION and slows down by STAGGER_FRICTION m/s².
const STAGGER_DURATION := 0.45
const STAGGER_FRICTION := 4.0
## A push this soon before a fall counts for the pusher.
const PUSH_CREDIT_TIME := 4.0

const ROLL_SPEED := 6.0
const ROLL_COOLDOWN := 1.2
const DODGE_DURATION := 0.18
const DODGE_SPEED := 12.0
const DODGE_COOLDOWN := 0.6
## Two presses of the same direction within this many seconds make a dodge.
const DOUBLE_TAP_TIME := 0.25
## The direction each move action dodges in when double-tapped, relative to the camera.
const DODGE_DIRECTIONS: Dictionary[StringName, Vector3] = {
	&"move_forward": Vector3.FORWARD,
	&"move_back": Vector3.BACK,
	&"move_left": Vector3.LEFT,
	&"move_right": Vector3.RIGHT,
}

## How long a player who came through the portal cannot be pushed, and how far from its centre
## they appear.
const SPAWN_PROTECTION := 1.5
const SPAWN_SCATTER := 0.8
## Below this height the player has fallen off for good.
const FALL_OUT_HEIGHT := -4.0

## Where the player appears when they come through the portal.
var spawn_position := Vector3.ZERO
## Seconds since the player fell off.
var out_time := 0.0
## Whether the player won the round: they dance while standing still.
var cheering := false

var _gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")
var _camera_yaw := 0.0
var _camera_pitch := -0.3
## Time left in the current push, roll, dodge or stagger. The player is free when it is zero.
var _action_time := 0.0
## Direction of the current roll or dodge.
var _action_direction := Vector3.ZERO
var _push_cooldown := 0.0
var _roll_cooldown := 0.0
var _dodge_cooldown := 0.0
var _protection_time := 0.0
## Move action -> when it was last pressed, to spot double taps.
var _last_tap_times: Dictionary[StringName, float] = {}
var _last_pusher_id := ""
var _last_pushed_time := -INF

@onready var camera_pivot: Node3D = $CameraPivot


static func _now() -> float:
	return Time.get_ticks_msec() / 1000.0


## The yaw that faces along [param direction].
static func _yaw_of(direction: Vector3) -> float:
	return atan2(-direction.x, -direction.z)


func _ready() -> void:
	super()
	name_tag.hide()
	camera_pivot.rotation.x = _camera_pitch


func _unhandled_input(event: InputEvent) -> void:
	var motion := event as InputEventMouseMotion
	if motion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		_camera_yaw -= motion.relative.x * MOUSE_SENSITIVITY
		_camera_pitch = clampf(_camera_pitch - motion.relative.y * MOUSE_SENSITIVITY, MIN_PITCH, MAX_PITCH)
		camera_pivot.rotation = Vector3(_camera_pitch, _camera_yaw, 0.0)


func _physics_process(delta: float) -> void:
	if is_out():
		out_time += delta
		return
	_count_down(delta)
	var dodge_direction := _read_double_tap()
	if _action_time == 0.0:
		_start_action_from_input(dodge_direction)
	_update_velocity(delta)
	move_and_slide()
	if global_position.y < FALL_OUT_HEIGHT:
		_fall_off()
		return
	if _action_time == 0.0:
		show_action(_locomotion_action())
	face(_yaw_of(_action_direction) if action == Action.ROLL else _camera_yaw)


## This player as the session replicates them.
func get_state() -> PlayerState:
	var state := PlayerState.new()
	state.position = global_position
	state.yaw = facing_yaw
	state.action = action
	state.player_name = player_name
	return state


## Applies a push that [param pusher_id] made from [param from], facing [param yaw], if it reaches
## this player and they are not rolling, dodging or fresh out of the portal.
func take_push(pusher_id: String, from: Vector3, yaw: float) -> void:
	if is_out() or action == Action.ROLL or action == Action.DODGE or _protection_time > 0.0:
		return
	var offset := global_position - from
	var flat_offset := Vector3(offset.x, 0.0, offset.z)
	var forward := Vector3.FORWARD.rotated(Vector3.UP, yaw)
	var in_reach := flat_offset.length() <= PUSH_RANGE and absf(offset.y) <= PUSH_HEIGHT
	var in_front := flat_offset.is_zero_approx() or forward.angle_to(flat_offset) <= deg_to_rad(PUSH_CONE_DEGREES)
	if not (in_reach and in_front):
		return
	velocity = forward * KNOCKBACK_SPEED + Vector3.UP * KNOCKBACK_LIFT
	_start(Action.STAGGER, STAGGER_DURATION)
	_last_pusher_id = pusher_id
	_last_pushed_time = _now()


## Brings the player back through the portal, safe from pushes for a moment.
func respawn() -> void:
	var scatter := Vector2.from_angle(randf() * TAU) * randf() * SPAWN_SCATTER
	global_position = spawn_position + Vector3(scatter.x, 0.0, scatter.y)
	velocity = Vector3.ZERO
	_action_time = 0.0
	_protection_time = SPAWN_PROTECTION
	show_action(Action.AIRBORNE)


func _count_down(delta: float) -> void:
	_action_time = maxf(_action_time - delta, 0.0)
	_push_cooldown = maxf(_push_cooldown - delta, 0.0)
	_roll_cooldown = maxf(_roll_cooldown - delta, 0.0)
	_dodge_cooldown = maxf(_dodge_cooldown - delta, 0.0)
	_protection_time = maxf(_protection_time - delta, 0.0)


func _start(timed_action: Action, duration: float) -> void:
	_action_time = duration
	show_action(timed_action)


func _start_action_from_input(dodge_direction: Vector3) -> void:
	if Input.is_action_just_pressed(&"push") and _push_cooldown == 0.0:
		_push_cooldown = PUSH_COOLDOWN
		_start(Action.PUSH, PUSH_DURATION)
		pushed.emit(global_position, _camera_yaw)
	elif Input.is_action_just_pressed(&"roll") and _roll_cooldown == 0.0 and is_on_floor():
		var input_direction := _input_direction()
		var rolls_forward := input_direction == Vector3.ZERO
		_action_direction = _camera_basis() * Vector3.FORWARD if rolls_forward else input_direction.normalized()
		_roll_cooldown = ROLL_COOLDOWN
		_start(Action.ROLL, ROLL_DURATION)
	elif dodge_direction != Vector3.ZERO and _dodge_cooldown == 0.0:
		_action_direction = dodge_direction
		_dodge_cooldown = DODGE_COOLDOWN
		_start(Action.DODGE, DODGE_DURATION)
	elif Input.is_action_just_pressed(&"jump") and is_on_floor():
		velocity.y = JUMP_VELOCITY


func _update_velocity(delta: float) -> void:
	if not is_on_floor():
		velocity.y -= _gravity * delta
	var horizontal := Vector3(velocity.x, 0.0, velocity.z)
	match action:
		Action.ROLL:
			horizontal = _action_direction * ROLL_SPEED
		Action.DODGE:
			horizontal = _action_direction * DODGE_SPEED
		Action.STAGGER:
			horizontal = horizontal.move_toward(Vector3.ZERO, STAGGER_FRICTION * delta)
		Action.PUSH:
			horizontal = horizontal.move_toward(Vector3.ZERO, ACCELERATION * delta)
		_:
			var acceleration := ACCELERATION * AIR_CONTROL
			if is_on_floor():
				# Only rolls, dodges and pushes move a player faster than a run on the ground.
				acceleration = ACCELERATION
				horizontal = horizontal.limit_length(MOVE_SPEED)
			horizontal = horizontal.move_toward(_input_direction() * MOVE_SPEED, acceleration * delta)
	velocity.x = horizontal.x
	velocity.z = horizontal.z


func _locomotion_action() -> Action:
	if not is_on_floor():
		return Action.AIRBORNE
	if Vector2(velocity.x, velocity.z).length() > 0.5:
		return Action.RUN
	return Action.CHEER if cheering else Action.IDLE


## The dodge direction when a move key was just pressed twice in quick succession, else zero.
func _read_double_tap() -> Vector3:
	var now := _now()
	for move: StringName in DODGE_DIRECTIONS:
		if not Input.is_action_just_pressed(move):
			continue
		if now - _last_tap_times.get(move, -INF) <= DOUBLE_TAP_TIME:
			_last_tap_times.erase(move)
			return _camera_basis() * DODGE_DIRECTIONS[move]
		_last_tap_times[move] = now
	return Vector3.ZERO


## The move keys' direction in the world, relative to where the camera looks.
func _input_direction() -> Vector3:
	var input := Input.get_vector(&"move_left", &"move_right", &"move_forward", &"move_back")
	return _camera_basis() * Vector3(input.x, 0.0, input.y)


func _camera_basis() -> Basis:
	return Basis(Vector3.UP, _camera_yaw)


func _fall_off() -> void:
	var pushed_recently := _now() - _last_pushed_time <= PUSH_CREDIT_TIME
	fell_off.emit(_last_pusher_id if pushed_recently else "")
	_last_pusher_id = ""
	out_time = 0.0
	_action_time = 0.0
	velocity = Vector3.ZERO
	# Wait unseen at the portal: the camera watches the platform from there.
	global_position = spawn_position
	show_action(Action.OUT)
