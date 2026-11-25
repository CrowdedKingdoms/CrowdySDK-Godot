class_name ArenaState
extends RefCounted
## What the referee decides for everyone in the arena, sent as one Crowdy event: the round, the
## platform's size and, once a round is won, its winner. The mode is not here: it is the match's,
## stored in its chunk (MatchConfig).

enum Phase { PLAYING, ROUND_OVER }

## Phase (byte), round number (u32), platform radius (float) and the winner id's length (byte),
## followed by the winner id.
const HEADER_SIZE := 10

var phase := Phase.PLAYING
var round_number := 1
var platform_radius := 0.0
## The actor who won the round, while the phase is ROUND_OVER.
var winner_id := ""


## Unpacks what [method encode] packed; null when [param bytes] is not an arena state.
static func decode(bytes: PackedByteArray) -> ArenaState:
	if bytes.size() < HEADER_SIZE or bytes.size() != HEADER_SIZE + bytes.decode_u8(9):
		return null
	if bytes.decode_u8(0) >= Phase.size():
		return null
	var state := ArenaState.new()
	state.phase = bytes.decode_u8(0) as Phase
	state.round_number = bytes.decode_u32(1)
	state.platform_radius = bytes.decode_float(5)
	state.winner_id = bytes.slice(HEADER_SIZE).get_string_from_ascii()
	return state


func encode() -> PackedByteArray:
	var winner_bytes := winner_id.to_ascii_buffer()
	var bytes := PackedByteArray()
	bytes.resize(HEADER_SIZE)
	bytes.encode_u8(0, phase)
	bytes.encode_u32(1, round_number)
	bytes.encode_float(5, platform_radius)
	bytes.encode_u8(9, winner_bytes.size())
	return bytes + winner_bytes


## A copy to change. Going through the encoding copies every field the event carries.
func copy() -> ArenaState:
	return ArenaState.decode(encode())


func equals(other: ArenaState) -> bool:
	return encode() == other.encode()
