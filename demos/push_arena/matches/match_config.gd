class_name MatchConfig
extends RefCounted
## A custom match as its chunk stores it: one voxel edit at VOXEL whose state holds the match's
## rules. Any player of the app can read it, so it is the source of truth for the match's mode.
## The match's referee writes it again every HEARTBEAT_INTERVAL seconds; the time the platform
## records for that edit tells the lobby whether the match is still being played.

enum Mode { ROUNDS, ENDLESS }
enum Visibility { PUBLIC, PRIVATE }

## Outside the 16 x 16 x 16 grid a block game draws, with a type no block uses.
const VOXEL := Vector3i(-1, 0, 0)
const VOXEL_TYPE := 1000
## The stored layout's version: a config written in another layout reads as no match.
const FORMAT := 1
## Format, mode, visibility, player count and the creator name's length (bytes), then the name.
const HEADER_SIZE := 5
const MAX_NAME_LENGTH := 16
## How often the referee writes the config, and how long after the last write the match counts
## as over.
const HEARTBEAT_INTERVAL := 30.0
const LIVE_TIMEOUT_MS := 120000

var mode := Mode.ENDLESS
var visibility := Visibility.PRIVATE
## Who created the match, shown in the lobby.
var creator_name := ""
## How many players were in the match when the config was last written.
var player_count := 0
## The chunk the match is in. Not stored: the voxel's own chunk.
var chunk := Vector3i.ZERO
## When the platform recorded the last write, in Unix milliseconds; 0 for a match not stored yet,
## one this player is creating. Not stored.
var written_at_ms := 0


## The config in a fetch_voxels() entry, or null when the entry is not a match config.
static func from_voxel(entry: Dictionary) -> MatchConfig:
	if entry.voxel != VOXEL or entry.voxel_type != VOXEL_TYPE:
		return null
	var config := decode(entry.state)
	if config:
		config.chunk = entry.chunk
		config.written_at_ms = entry.updated_at_ms
	return config


## Unpacks what [method encode] packed; null when [param bytes] are not a config.
static func decode(bytes: PackedByteArray) -> MatchConfig:
	if bytes.size() < HEADER_SIZE or bytes.decode_u8(0) != FORMAT:
		return null
	if bytes.size() != HEADER_SIZE + bytes.decode_u8(4):
		return null
	if bytes.decode_u8(1) >= Mode.size() or bytes.decode_u8(2) >= Visibility.size():
		return null
	var config := MatchConfig.new()
	config.mode = bytes.decode_u8(1) as Mode
	config.visibility = bytes.decode_u8(2) as Visibility
	config.player_count = bytes.decode_u8(3)
	config.creator_name = bytes.slice(HEADER_SIZE).get_string_from_utf8()
	return config


func encode() -> PackedByteArray:
	var name_bytes := creator_name.left(MAX_NAME_LENGTH).to_utf8_buffer()
	var bytes := PackedByteArray()
	bytes.resize(HEADER_SIZE)
	bytes.encode_u8(0, FORMAT)
	bytes.encode_u8(1, mode)
	bytes.encode_u8(2, visibility)
	bytes.encode_u8(3, mini(player_count, 255))
	bytes.encode_u8(4, name_bytes.size())
	return bytes + name_bytes


## Whether the match was written within LIVE_TIMEOUT_MS, by this client's clock.
func is_live() -> bool:
	return Time.get_unix_time_from_system() * 1000.0 - written_at_ms < LIVE_TIMEOUT_MS
