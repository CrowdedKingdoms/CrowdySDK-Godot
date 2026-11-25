class_name Referee
extends Node
## Keeps the arena's rules. The platform's size follows how many players stand on it, and in
## Rounds mode a player who stands on it alone for WIN_TIME wins the round. One player of the
## match decides (see PushArena); everyone else adopts the states they broadcast, so all see the
## same arena.

## The arena state changed: decided here, or adopted from the referee.
signal state_changed(previous: ArenaState)

## The platform's radius is MIN_RADIUS plus RADIUS_PER_PLAYER times the square root of the
## players on it, so its area grows with the crowd.
const MIN_RADIUS := 3.0
const RADIUS_PER_PLAYER := 1.5
## How long a player must stand on the platform alone to win a round.
const WIN_TIME := 3.0
## How long the winner celebrates before the next round starts.
const CELEBRATION_TIME := 4.0
## How long a player who fell off waits before they come back through the portal. Longer than
## WIN_TIME, so the last player standing can win before the others return.
const RESPAWN_DELAY := 4.0

## The match's mode, set before the first decision.
var mode := MatchConfig.Mode.ENDLESS
## The arena's current state on this client.
var state := ArenaState.new()

var _alone_time := 0.0
var _celebration_time := 0.0


static func platform_radius_for(players_on_platform: int) -> float:
	return MIN_RADIUS + RADIUS_PER_PLAYER * sqrt(maxi(players_on_platform, 1))


func _init() -> void:
	state.platform_radius = platform_radius_for(1)


## Decides the next state from the players on the platform ([param alive_ids]) and the number of
## players in the arena. Only the referee calls it, every physics frame.
func decide(delta: float, alive_ids: PackedStringArray, player_count: int) -> void:
	var next := state.copy()
	next.platform_radius = platform_radius_for(alive_ids.size())
	if mode == MatchConfig.Mode.ROUNDS:
		_decide_round(next, delta, alive_ids, player_count)
	_change_state(next)


## Takes the state the referee broadcast.
func adopt(received: ArenaState) -> void:
	_change_state(received)


func _decide_round(next: ArenaState, delta: float, alive_ids: PackedStringArray, player_count: int) -> void:
	if next.phase == ArenaState.Phase.ROUND_OVER:
		_celebration_time += delta
		if _celebration_time >= CELEBRATION_TIME:
			_start_next_round(next)
		return
	var last_standing := alive_ids.size() == 1 and player_count >= 2
	_alone_time = _alone_time + delta if last_standing else 0.0
	if _alone_time >= WIN_TIME:
		next.phase = ArenaState.Phase.ROUND_OVER
		next.winner_id = alive_ids[0]
		_celebration_time = 0.0


func _start_next_round(next: ArenaState) -> void:
	next.round_number += 1
	next.phase = ArenaState.Phase.PLAYING
	next.winner_id = ""
	_alone_time = 0.0


func _change_state(next: ArenaState) -> void:
	if next.equals(state):
		return
	var previous := state
	state = next
	state_changed.emit(previous)
