# Push Arena

Push the other players off a round platform. The platform grows and shrinks with the number of
players standing on it. Players who fall come back through the portal in the middle after a few
seconds. Play it from the main menu: **Push Arena > Online** (sign in first) or **Offline** (alone,
Endless).

## Controls

| Input | Action |
|---|---|
| WASD or arrow keys, mouse | Move, look and aim |
| Left click | Push: both hands forward. A player in front of you is flung back a little. |
| Space | Jump |
| Shift | Roll in the direction you move. You can't be pushed while rolling. |
| Double-tap a direction | Dodge: a quick step left, right, forward or back. You can't be pushed while dodging. |
| Esc | Leave: back to the lobby online, to the main menu offline |

## Matches

Online, the game starts in the lobby:

- **The public arena** is always Endless and always open.
- **New match**: pick Rounds or Endless, and Public or Private. A public match takes a free slot in
  a row of known chunks beside the public arena, and the lobby lists it. A private match goes to a
  random chunk far from all of them. Its coordinates show in the HUD (for example "Private match
  at 483201, -772915"); share them, and others join with **Private match > Join**.
- A match's chunk stores its config, so everyone who joins reads the same mode from it. The match's
  referee writes the config again every 30 s with the player count. A match not written for 2
  minutes is over: it leaves the list, and its slot or chunk can be taken by a new match.

## Rules

- **Rounds**: whoever stands alone on the platform for 3 s, while at least one other player is in
  the arena, wins the round. After 4 s of celebration, everyone who fell comes back for the next
  round.
- **Endless**: no rounds. Your score is the time you spend on the platform, and alone on it.
- A fallen player comes back after 4 s. That is longer than the 3 s needed to win, so the last
  one standing can win before the others return.
- After coming through the portal, a player can't be pushed for 1.5 s.
- A fall within 4 s of a push counts for the pusher.

The HUD shows your stats for this visit: time on the platform, time alone on it, players pushed
off, falls, best run (the longest stay without falling) and rounds won. Online it also shows the
arena's **records**: the longest run, the most time alone, the most players pushed off and the most
rounds won in one visit, each with its holder. Every arena keeps its own, and a new match starts
with none; the public arena's are the global board. A feed lists falls, wins, new record holders, arrivals and departures.

## Crowdy features it shows

| Feature | Where |
|---|---|
| Sign-in, the checks a new account must pass, starting a session with a `CrowdySessionConfig` | `demos/common/login_menu.gd`, `lobby/arena_lobby.gd` |
| Chunks as rooms: each arena is one chunk; players in different arenas are out of each other's range | `matches/match_directory.gd`, `_join()` in `arena.gd` |
| Voxel state as the world's storage: a match's config and the records are voxel edits in its chunk, written with `send_voxel_update` and heard live through `voxel_updated` | `matches/match_config.gd`, `rules/arena_records.gd`, `arena.gd` |
| Reading stored voxels with `fetch_voxels`: one box read lists every public match; one chunk read joins a match or loads its records | `matches/match_directory.gd`, `arena.gd` |
| Actor state replication: position, facing, action and name in at most 82 bytes, sent with `set_actor_state` (only when changed) | `player/player_state.gd`, `arena.gd` |
| Remote actors from `actor_joined`, `actor_updated`, `actor_left` and `get_remote_actors()` | `arena.gd`, `player/remote_arena_player.gd` |
| Typed events with `send_event` and `event_received`: `PUSH`, `FELL_OFF`, `ARENA_STATE` | `arena.gd` |
| Leaving a chunk by ending the session: the others are told at once | `_unhandled_input()` in `arena.gd` |
| Idle timeout recovery (`reconnect()` on input), and `send_rejected` shown in the feed | `arena.gd` |

Physics stays with each player's own client. A push is a broadcast event that carries the
pusher's position and facing. Each receiving client checks whether the push reached its own
player and applies the knockback itself.

Each arena has one referee, who decides the platform's size and the rounds and broadcasts them.
Every client picks the same one without any traffic: the player in the arena with the lowest
actor id. Crowdy's own host election (`host_tracking`) can't do this job, because it elects one
host per app, not per chunk: with several matches at once, only one would have a referee.

Players can write voxels because new Crowdy apps are open by default: every player gets
`update_voxel_data` on the world grid. An app that closes its wilderness, or a grid over these
chunks that refuses building, makes `send_voxel_update` fail with `UNAUTHORIZED` (shown in the
feed), and then matches can't be created or records saved. Chunk-level state (`chunkState`) would
also fit, but only app admins (`manage_apps`) may write it.

## Animations

From `demos/common/assets/UAL1_Standard.glb`: `Idle`, `Jog_Fwd`, `Jump`, `Push`, `Roll`,
`Hit_Chest` (when pushed) and `Dance` (the round winner).

Missing from the library:

- **Dodge**: there is no sidestep or dash clip, so a dodging player keeps their current clip.
- **A quick two-handed shove**: `Push` is a loop of pushing a heavy object. The push plays its
  first 0.4 s, which keeps both arms straight out, but it is not a shove.

## Test it online

Run two instances, for example with Debug > Customize Run Instances in the editor, and sign in
on both (one account for both works). Create a match in one, then join it from the other: from
the list if it is public, or with its coordinates if it is private.

## Files

| Path | Role |
|---|---|
| `arena.tscn`, `arena.gd` | `PushArena`: the scene, and what talks to Crowdy during play. |
| `lobby/` | `ArenaLobby`: the public arena, the public match list, new and private matches. |
| `matches/` | `MatchConfig` (a match as its chunk stores it), `MatchDirectory` (where matches are, and finding them). |
| `player/` | `ArenaPlayer` (model and clips), `LocalArenaPlayer` (input and physics), `RemoteArenaPlayer` (smoothing), `PlayerState` (the replicated bytes). |
| `rules/` | `Referee` (what the referee decides), `ArenaState` (the event it sends), `ArenaRecords`, `PlayerStats`. |
| `stage/arena_platform.gd` | The platform that resizes. |
| `hud/` | Stats, records, where the arena is, banner and feed. |
| `session_config.tres` | 20 updates per second. |
| `demo.tres` | How the main menu lists the demo, and the lobby it starts online in. |
