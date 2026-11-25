class_name ArenaPlatform
extends StaticBody3D
## The round platform the players stand on. Its edge moves toward the radius the referee decides
## at a walking pace, so players near the edge can step back before it shrinks under them.

## How fast the edge moves, in metres per second.
const EDGE_SPEED := 1.0

var _target_radius := 0.0

@onready var _shape: CylinderShape3D = $CollisionShape3D.shape
@onready var _mesh: CylinderMesh = $Mesh.mesh


func _ready() -> void:
	_target_radius = _shape.radius


func _physics_process(delta: float) -> void:
	if _shape.radius == _target_radius:
		return
	var radius := move_toward(_shape.radius, _target_radius, EDGE_SPEED * delta)
	_shape.radius = radius
	_mesh.top_radius = radius
	_mesh.bottom_radius = radius


func resize(radius: float) -> void:
	_target_radius = radius
