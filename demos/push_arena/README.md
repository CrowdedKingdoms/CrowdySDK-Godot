# Push Arena

Push the other players off a round platform. The platform grows and shrinks with the number of
players standing on it. Players who fall come back through the portal in the middle after a few
seconds. Play it from the main menu: **Push Arena > Online** (sign in first) or **Offline** (alone).

## Controls

| Input | Action |
|---|---|
| WASD or arrow keys, mouse | Move, look and aim |
| Left click | Push: both hands forward. A player in front of you is flung back a little. |
| Space | Jump |
| Shift | Roll in the direction you move. You can't be pushed while rolling. |
| Double-tap a direction | Dodge: a quick step left, right, forward or back. You can't be pushed while dodging. |
| M | Switch between Rounds and Endless (host only, or offline) |
| Esc | Back to the main menu |

## Rules

- **Rounds** (the default): whoever stands alone on the platform for 3 s, while at least one
  other player is in the arena, wins the round. After 4 s of celebration, everyone who fell comes
  back for the next round.
- **Endless**: no rounds. Your score is the time you spend on the platform, and alone on it.
- A fallen player comes back after 4 s. That is longer than the 3 s needed to win, so the last
  one standing can win before the others return.
- After coming through the portal, a player can't be pushed for 1.5 s.
- A fall within 4 s of a push counts for the pusher.

The HUD shows your stats for this visit: time on the platform, time alone on it, players pushed
off, falls, best run (the longest stay without falling) and rounds won. A feed lists falls, wins,
arrivals and departures.

## Crowdy features it shows

| Feature | Where |
|---|---|
| Sign-in, the checks a new account must pass, starting a session with a `CrowdySessionConfig` | `demos/common/login_menu.gd`, `session_config.tres` |
| Actor state replication: position, facing, action and name in at most 82 bytes, sent with `set_actor_state` (only when changed) | `player/player_state.gd`, `arena.gd` |
| Remote actors from `actor_joined`, `actor_updated`, `actor_left` and `get_remote_actors()` | `arena.gd`, `player/remote_arena_player.gd` |
| Typed events with `send_event` and `event_received`: `PUSH`, `FELL_OFF`, `ARENA_STATE` | `arena.gd` |
| Host tracking: the host (`is_host()`) decides the platform size, the rounds and the mode, and broadcasts them. When the host leaves, the next host carries on from the last state. | `rules/referee.gd`, `rules/arena_state.gd` |
| Chunks as rooms: every arena player reports one chunk, 2 km from the shared_world demo's players, so the two demos never see each other | `ARENA_CHUNK` in `arena.gd` |
| Idle timeout recovery (`reconnect()` on input) and `send_rejected` warnings | `arena.gd` |

Physics stays with each player's own client. A push is a broadcast event that carries the
pusher's position and facing. Each receiving client checks whether the push reached its own
player and applies the knockback itself. Events are not echoed to their sender, so the host also
applies its own decisions locally.

## Animations

From `demos/common/assets/UAL1_Standard.glb`: `Idle`, `Jog_Fwd`, `Jump`, `Push`, `Roll`,
`Hit_Chest` (when pushed) and `Dance` (the round winner).

Missing from the library:

- **Dodge**: there is no sidestep or dash clip, so a dodging player keeps their current clip.
- **A quick two-handed shove**: `Push` is a loop of pushing a heavy object. The push plays its
  first 0.4 s, which keeps both arms straight out, but it is not a shove.

## Test it online

Run two instances, for example with Debug > Customize Run Instances in the editor. Sign in with
**two different accounts**: the host is elected per account, so two instances of one account would
both act as host.

## Files

| Path | Role |
|---|---|
| `arena.tscn`, `arena.gd` | The scene, and the only script that talks to Crowdy. |
| `player/` | `ArenaPlayer` (model and clips), `LocalArenaPlayer` (input and physics), `RemoteArenaPlayer` (smoothing), `PlayerState` (the replicated bytes). |
| `rules/` | `Referee` (what the host decides), `ArenaState` (the event it sends), `PlayerStats`. |
| `stage/arena_platform.gd` | The platform that resizes. |
| `hud/` | Stats, mode, banner and feed. |
| `session_config.tres` | 20 updates per second, host tracking on. |
| `demo.tres` | How the main menu lists the demo. |
