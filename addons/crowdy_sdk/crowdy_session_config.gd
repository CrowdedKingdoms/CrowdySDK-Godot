class_name CrowdySessionConfig
extends Resource
## Tunes a [CrowdyGameSession] for one game: how often and how far the local actor is sent, and
## how the game's world maps onto Crowdy chunks. Pass it to [method Crowdy.start_session].

## Edge length of one Crowdy chunk in world units. The realtime servers fan updates out by
## chunk, so with [member distance] this decides who counts as nearby.
@export var chunk_size := 16.0
## How many times per second the local actor's state is sent while it changes.
@export_range(1, 60) var send_rate_hz := 5
## How far, in chunks, the local actor's updates, events and text reach.
@export_range(0, 8) var distance := 8
## A full update is sent at least this often, even when nothing changed.
@export_range(100, 60000, 1, "or_greater", "suffix:ms") var keyframe_interval_ms := 3000
## While nothing changes, a cheap heartbeat keeps the actor present this often (0 disables).
@export_range(0, 60000, 1, "or_greater", "suffix:ms") var heartbeat_interval_ms := 2000
## A remote actor not heard from for this long is reported as left.
@export_range(1000, 120000, 1, "or_greater", "suffix:ms") var stale_after_ms := 12000
## Track which player is the app's host (see [signal CrowdyGameSession.host_changed]). Costs
## two API requests every 3 seconds.
@export var host_tracking := false
## Keep this actor id (32 characters) across sessions. Empty picks a new one each session.
@export var actor_id := ""
## Server assignment attempts. Small deployments can briefly have no server available.
@export_range(1, 10) var connect_attempts := 1
## How long to wait for the connection to be ready before start fails with TIMEOUT.
@export_range(0, 60000, 1, "or_greater", "suffix:ms") var connect_timeout_ms := 5000


## The chunk containing [param position].
func world_to_chunk(position: Vector3) -> Vector3i:
	return Vector3i((position / chunk_size).floor())


## The settings as [method CrowdyGameSession.start] takes them.
func to_dictionary() -> Dictionary:
	var settings := {
		"send_rate_hz": send_rate_hz,
		"distance": distance,
		"keyframe_interval_ms": keyframe_interval_ms,
		"heartbeat_interval_ms": heartbeat_interval_ms,
		"stale_after_ms": stale_after_ms,
		"host_tracking": host_tracking,
		"connect_attempts": connect_attempts,
		"connect_timeout_ms": connect_timeout_ms,
	}
	if not actor_id.is_empty():
		settings["actor_id"] = actor_id
	return settings
