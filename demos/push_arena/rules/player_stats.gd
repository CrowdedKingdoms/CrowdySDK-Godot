class_name PlayerStats
extends RefCounted
## The local player's scores during this visit to the arena. All of them are counted on this
## client: times from its own player and the replicated states of the others, pushes and wins from
## the arena's events.

var time_on_platform := 0.0
## Time as the only player on the platform while others were in the arena.
var time_alone := 0.0
## Players this player pushed off the platform.
var players_pushed_off := 0
var falls := 0
## The longest stay on the platform without falling, and the current one.
var best_run := 0.0
var current_run := 0.0
var rounds_won := 0


## Counts [param delta] seconds: on the platform when [param on_platform], and alone on it when
## [param alone].
func track(delta: float, on_platform: bool, alone: bool) -> void:
	if not on_platform:
		return
	time_on_platform += delta
	current_run += delta
	best_run = maxf(best_run, current_run)
	if alone:
		time_alone += delta


func record_fall() -> void:
	falls += 1
	current_run = 0.0
