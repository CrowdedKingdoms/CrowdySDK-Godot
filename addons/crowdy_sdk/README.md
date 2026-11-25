# Crowdy SDK for Godot

Crowded Kingdoms accounts and realtime multiplayer for Godot 4.7, as a GDExtension over
[CrowdyCPP](https://github.com/CrowdedKingdoms/CrowdyCPP) (v0.93). It wraps the native UDP
replication client and CrowdyCPP's `WorldSession`, so a game gets presence, remote-actor tracking,
events, chat and stored voxel state without writing netcode bookkeeping.

## Install

1. Copy `addons/crowdy_sdk/` into the project, with the libraries built into its `bin/` folder
   (see `native/README.md` in this repository).
2. Enable **Crowdy SDK** in Project Settings > Plugins. It registers the `Crowdy` autoload and the
   project settings below.
3. Set the project settings:

| Setting | Meaning |
|---|---|
| `crowdy/api_url` | API origin, e.g. `https://ck.dev.crowdedkingdoms.com`. Empty uses the SDK's default (prod). |
| `crowdy/app_id` | The app the game plays, used by `Crowdy.start_session()`. |
| `crowdy/persist_session` | Keep the player signed in across launches (a session file in `user://`). Default `true`. |

| File | Role |
|---|---|
| `crowdy.gd` | The `Crowdy` autoload: the entry point games use. |
| `crowdy_session_config.gd` | `CrowdySessionConfig`, a resource that tunes a session for one game. |
| `plugin.cfg`, `plugin.gd` | The editor plugin: registers the autoload and the settings. |
| `crowdy_sdk.gdextension` | Tells Godot which native library to load, and which DLLs an export ships. |
| `bin/` | The native library and the DLLs it loads (built from `native/`). |

## Quick start

```gdscript
const SESSION_CONFIG := preload("res://my_session_config.tres")  # a CrowdySessionConfig

func _on_login_pressed() -> void:
	var result: CrowdyResult = await Crowdy.identity.sign_in(email, password).completed
	if result.ok:
		result = await Crowdy.start_session(SESSION_CONFIG)
	if not result.ok:
		status.text = result.error_message
		return
	var chunk := SESSION_CONFIG.world_to_chunk(player.global_position)
	Crowdy.session.join(chunk, player.encode_state())

func _physics_process(_delta: float) -> void:
	if Crowdy.session.has_joined():
		Crowdy.session.set_actor_state(player.encode_state())   # sent only when it changed
		Crowdy.session.move_to_chunk(SESSION_CONFIG.world_to_chunk(player.global_position))
```

Remote players come from `Crowdy.session` signals: `actor_joined`, `actor_updated` and
`actor_left`. A scene loaded after the session started reads the actors already present with
`get_remote_actors()`. `demos/shared_world/world.gd` in this repository is a complete example.

## How it works

- **Everything asynchronous returns a `CrowdyRequest`.** Await its `completed` signal right after
  the call; it delivers a `CrowdyResult` with `ok`, `data`, `error_code` and `error_message`.
  Results are always delivered from a later frame, never during the call.
- **One thread for scripts.** Every signal and result is emitted on the main thread from
  `poll()`, which the `Crowdy` autoload calls every frame, also while the game is paused. Blocking
  work (HTTP, connecting) runs on SDK threads.
- **Binary state stays binary.** Actor states and payloads are `PackedByteArray`s, sent as-is.
  An actor state is at most 256 bytes; pack it (`encode_float`, `encode_u8`, ...) rather than
  sending JSON.
- **Two tokens.** `Crowdy.identity` holds the player's session token (sign-in). A game session
  runs on an app token minted from it (`mint_app_token`), on the app's own datacenter.
  `Crowdy.start_session()` does the mint and the start.

## Before a new account can play

The platform checks every app-token mint. Branch on `error_code`:

| `error_code` | What the player does | SDK call |
|---|---|---|
| `EMAIL_CONFIRMATION_REQUIRED` | Opens the link emailed at registration. | `identity.resend_confirmation_email(email)` |
| `LEGAL_ACCEPTANCE_REQUIRED` | Accepts the documents you show them. | `identity.fetch_legal_documents(app_id)`, then `identity.accept_legal_documents(app_id)` |
| `AGE_REQUIRED` | Gives a birth date and country. | `identity.record_player_age(birth_date, country, parental_confirmation)` |

Record acceptance only after the player accepted the documents in your UI.

## API

### `Crowdy` (autoload)

| Member | Description |
|---|---|
| `identity: CrowdyIdentity` | The player's account. |
| `session: CrowdyGameSession` | The realtime session; lives as long as the game, so connected signals survive restarts of the session. |
| `start_session(config: CrowdySessionConfig = null, app_id := "") -> CrowdyResult` | Mints an app token and starts `session`. A coroutine: `await` it. |

### `CrowdyIdentity`

Created with `CrowdyIdentity.create(api_url, persist_session)`; `Crowdy.identity` is one.

| Method | `data` on success |
|---|---|
| `sign_in(email, password)` | `{user_id, email, gamertag}` |
| `register_user(email, password, gamertag = "")` | `{user_id, email, gamertag}` |
| `sign_out()` | — (the local session ends immediately) |
| `fetch_profile()` | `{user_id, email, gamertag, disambiguation, is_confirmed, age_bracket, country}` |
| `resend_confirmation_email(email)` | — |
| `request_password_reset(email)` | — |
| `fetch_legal_documents(app_id = "")` | Array of `{document_id, version, title, url, app_id}`: the platform's, plus the app's own when `app_id` is given |
| `accept_legal_documents(app_id = "")` | — |
| `record_player_age(birth_date, country, parental_confirmation = false)` | `{age_bracket, country}` |
| `mint_app_token(app_id)` | `CrowdyAppToken` (opaque; `app_id`, `expires_at`, `paused`) |
| `is_signed_in() -> bool` | Whether a session token is held (it may have expired: `fetch_profile()` checks). |

### `CrowdyGameSession`

| Method | Description |
|---|---|
| `start(token, config := {}) -> CrowdyRequest` | Connects to a realtime server. Fails with `BUSY` while started. |
| `stop()` | Leaves. Emits `actor_left` for every remote actor, then `state_changed(STATE_STOPPED)`. |
| `reconnect() -> CrowdyRequest` | After `STATE_INACTIVE` (idle timeout) or `STATE_FAILED`; call it on the player's next input. |
| `get_state() -> State` | `STATE_STOPPED`, `STATE_CONNECTING`, `STATE_CONNECTED`, `STATE_RECONNECTING`, `STATE_INACTIVE`, `STATE_FAILED`. |
| `join(chunk, state) -> Error` | Makes the local actor appear. |
| `set_actor_state(state)` | Replicated at the configured rate, only when changed; keyframes and heartbeats keep the actor present. |
| `move_to_chunk(chunk) -> Error` | Call every frame; sends at once when the chunk changes. |
| `get_actor_id() -> String` | The local actor's 32-character id. |
| `get_remote_actors() -> Array` | `{actor_id, chunk, state, server_time_ms}` for every actor in range. |
| `send_event(event_type, payload) -> Error` | A typed event (0–65535) to nearby actors: game commands, emotes. |
| `send_text(text) -> Error` | Proximity chat. |
| `send_channel_message(channel_id, payload) -> Error` | To a channel the player is a member of. |
| `send_direct_message(actor_id, chunk, payload) -> Error` | To one actor. The receiver is not told the sender; put it in the payload. |
| `send_voxel_update(chunk, voxel, voxel_type, state := PackedByteArray()) -> Error` | Stores one voxel edit in `chunk` and sends it to nearby actors. See [Voxels](#voxels). |
| `fetch_voxels(low_chunk, high_chunk) -> CrowdyRequest` | The stored voxel edits of a box of chunks. See [Voxels](#voxels). |
| `is_host()`, `get_host_user_id()` | With `host_tracking` on. |
| `get_stats() -> Dictionary` | Local traffic counters for a debug overlay (not billing figures). |

Signals: `state_changed(state)`, `actor_joined(actor_id, chunk, state)`,
`actor_updated(actor_id, chunk, state, server_time_ms)`, `actor_left(actor_id)`,
`event_received(event_type, sender_id, payload, from_server)`, `text_received(sender_id, text)`,
`channel_message_received(channel_id, sender_id, payload)`, `direct_message_received(payload)`,
`voxel_updated(chunk, voxel, voxel_type, state, sender_id)`, `host_changed(host_user_id)`,
`send_rejected(error_code, send_kind)`.

`actor_updated` also follows every `actor_joined`. `send_rejected` reports a send the server
refused, e.g. `UNAUTHORIZED` for text in an app without the text-chat permission; `send_kind` is
`actor_update`, `event`, `text`, `channel_message`, `direct_message`, `voxel_update`, or
`unknown`.

#### Voxels

Voxel edits are the world's stored state: each one persists in its chunk, and any player of the
app can read it. A game that draws no blocks can still store a place's data in them, such as a
match's rules or an arena's records (see `demos/push_arena`).

- `send_voxel_update(chunk, voxel, voxel_type, state)` stores one edit and sends it to the actors
  near `chunk`. Positions and types are any value from -32768 to 32767 (a block game uses 0–15 on
  each axis), and `state` is at most 1024 bytes. A newer edit at the same position replaces the
  older one. Writing needs `update_voxel_data` on the chunk: a new app grants it to every player,
  but a closed wilderness, another player's claim or a zone can refuse it (`send_rejected` with
  `UNAUTHORIZED`).
- `voxel_updated` reports the edits near the local actor as they happen, this player's own
  included once the server has accepted them.
- `fetch_voxels(low_chunk, high_chunk)` reads the stored edits of every chunk in a box, corners
  included, at most 4,913 chunks (17 × 17 × 17). `data` is an Array with one Dictionary per voxel
  position: `{chunk, voxel, voxel_type, state, updated_at_ms, user_id}`, where `updated_at_ms` is
  when the platform recorded the edit (Unix milliseconds) and `user_id` is who made it.

Chunk-level state (`chunkState`) is not wrapped: only app admins (`manage_apps`) may write it.

### `CrowdySessionConfig`

A `Resource` (create it in the FileSystem dock) with `chunk_size`, `send_rate_hz`, `distance`,
`keyframe_interval_ms`, `heartbeat_interval_ms`, `stale_after_ms`, `host_tracking`, `actor_id`,
`connect_attempts` and `connect_timeout_ms`, each documented in the inspector.
`world_to_chunk(position)` maps a world position to its chunk.

## Not wrapped yet

CrowdyCPP has more than this extension exposes: the voxel grid store (`ChunkStore`: dense
16 × 16 × 16 grids, loading them around the player, writing them back), grid claims, voice and
video, channel and team management, the Game Kit (matchmaking, lobbies, leaderboards on ck-exec)
and social sign-in. Each fits as another class beside `CrowdyIdentity` and `CrowdyGameSession`.
