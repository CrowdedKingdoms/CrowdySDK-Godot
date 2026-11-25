class_name ArenaPlayer
extends CharacterBody3D
## A player in the push arena: the body, the mannequin and the clip each action plays. The local
## player drives it from input; remote players from the states the realtime session receives.

## What a player is doing. It is replicated with the player's state and picks the clip their
## mannequin plays.
enum Action { IDLE, RUN, AIRBORNE, PUSH, ROLL, DODGE, STAGGER, CHEER, OUT }

## How long a roll lasts, in seconds. The roll clip is sped up to fit.
const ROLL_DURATION := 0.7
## The clip each action plays. The mannequin has no dodge clip, so a dodging player keeps playing
## their current one; a player who is OUT is hidden instead.
const CLIPS: Dictionary[Action, StringName] = {
	Action.IDLE: &"Idle",
	Action.RUN: &"Jog_Fwd",
	Action.AIRBORNE: &"Jump",
	Action.PUSH: &"Push",
	Action.ROLL: &"Roll",
	Action.STAGGER: &"Hit_Chest",
	Action.CHEER: &"Dance",
}
## Seconds over which one clip blends into the next.
const BLEND_TIME := 0.1

## The name other players see.
var player_name := "Player"
## What the player is doing, as shown.
var action := Action.IDLE
## Which way the player faces, as a rotation around the vertical axis (0 faces -Z).
var facing_yaw := 0.0

@onready var model: Node3D = $Model
@onready var animation_player: AnimationPlayer = $Model/AnimationPlayer
@onready var body_mesh: MeshInstance3D = $Model/Armature/Skeleton3D/Mannequin
@onready var collision_shape: CollisionShape3D = $CollisionShape3D
@onready var name_tag: Label3D = $NameTag


## A colour for the player with [param actor_id], the same on every client.
static func color_for(actor_id: String) -> Color:
	return Color.from_hsv(float(actor_id.hash() % 360) / 360.0, 0.6, 0.95)


func _ready() -> void:
	face(facing_yaw)
	animation_player.play(CLIPS[action])


func is_out() -> bool:
	return action == Action.OUT


## Shows [param new_action]: plays its clip, or hides the player while they are out.
func show_action(new_action: Action) -> void:
	if new_action == action:
		return
	action = new_action
	visible = not is_out()
	collision_shape.set_deferred(&"disabled", is_out())
	if not CLIPS.has(action):
		return
	var speed := 1.0
	if action == Action.ROLL:
		speed = animation_player.get_animation(CLIPS[action]).length / ROLL_DURATION
	animation_player.play(CLIPS[action], BLEND_TIME, speed)


## Turns the mannequin to face [param yaw].
func face(yaw: float) -> void:
	facing_yaw = yaw
	# The mannequin is modelled facing +Z, which is Godot's back.
	model.rotation.y = yaw + PI


## Tints the mannequin's body; its joints keep their colour.
func paint(color: Color) -> void:
	var material: StandardMaterial3D = body_mesh.get_active_material(0).duplicate()
	material.albedo_color = color
	body_mesh.set_surface_override_material(0, material)
