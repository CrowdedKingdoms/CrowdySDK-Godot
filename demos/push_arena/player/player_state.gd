class_name PlayerState
extends RefCounted
## One arena player as the realtime session replicates them: position, facing, action and name.
## Packed into at most 82 bytes; an actor state may be up to 256.

## Names are cut to this many characters (up to 4 bytes each in UTF-8).
const MAX_NAME_LENGTH := 16
## Position (3 floats), yaw (float), action (byte) and the name's length in bytes (byte), followed
## by the name.
const HEADER_SIZE := 18

var position := Vector3.ZERO
var yaw := 0.0
var action := ArenaPlayer.Action.IDLE
var player_name := ""


## Unpacks what [method encode] packed. Null when [param bytes] is not an arena player's state,
## such as the state of another game's actor.
static func decode(bytes: PackedByteArray) -> PlayerState:
	if bytes.size() < HEADER_SIZE or bytes.size() != HEADER_SIZE + bytes.decode_u8(17):
		return null
	var action_index := bytes.decode_u8(16)
	if action_index >= ArenaPlayer.Action.size():
		return null
	var state := PlayerState.new()
	state.position = Vector3(bytes.decode_float(0), bytes.decode_float(4), bytes.decode_float(8))
	state.yaw = bytes.decode_float(12)
	state.action = action_index as ArenaPlayer.Action
	state.player_name = bytes.slice(HEADER_SIZE).get_string_from_utf8()
	return state


func encode() -> PackedByteArray:
	var name_bytes := player_name.left(MAX_NAME_LENGTH).to_utf8_buffer()
	var bytes := PackedByteArray()
	bytes.resize(HEADER_SIZE)
	bytes.encode_float(0, position.x)
	bytes.encode_float(4, position.y)
	bytes.encode_float(8, position.z)
	bytes.encode_float(12, yaw)
	bytes.encode_u8(16, action)
	bytes.encode_u8(17, name_bytes.size())
	return bytes + name_bytes
