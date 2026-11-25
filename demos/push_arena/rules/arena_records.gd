class_name ArenaRecords
extends RefCounted
## The arena's high scores: one record per stat, each a voxel edit in the arena's chunk holding
## the value and who set it. A player who beats a record writes it; everyone in the arena hears
## the edit at once, and players who arrive later read it from the platform.

enum Stat { LONGEST_RUN, TIME_ALONE, PUSHED_OFF, ROUNDS_WON }

## What each Stat is called on screen.
const NAMES: Array[String] = ["Longest run", "Time alone", "Players pushed off", "Rounds won"]
## Record voxels sit outside the 16 x 16 x 16 grid, one per stat at (VOXEL_X, stat, 0).
const VOXEL_X := -2
const VOXEL_TYPE := 1001
## The stored layout's version: a record written in another layout is ignored.
const FORMAT := 1
## Format (byte), value (float) and the holder name's length (byte), then the name.
const HEADER_SIZE := 6
const MAX_NAME_LENGTH := 16

## Per Stat: the record value, and who holds it ("" while nobody does).
var values: Array[float] = []
var holders: Array[String] = []


## The voxel that stores [param stat]'s record.
static func voxel_of(stat: Stat) -> Vector3i:
	return Vector3i(VOXEL_X, stat, 0)


## The stat whose record a voxel edit holds, or -1 when the edit is not a record.
static func stat_of(voxel: Vector3i, voxel_type: int) -> int:
	if voxel_type != VOXEL_TYPE or voxel.x != VOXEL_X or voxel.z != 0:
		return -1
	return voxel.y if voxel.y >= 0 and voxel.y < Stat.size() else -1


## The value [param stats] reach in [param stat].
static func value_in(stats: PlayerStats, stat: Stat) -> float:
	match stat:
		Stat.LONGEST_RUN:
			return stats.best_run
		Stat.TIME_ALONE:
			return stats.time_alone
		Stat.PUSHED_OFF:
			return stats.players_pushed_off
		_:
			return stats.rounds_won


func _init() -> void:
	values.resize(Stat.size())
	values.fill(0.0)
	holders.resize(Stat.size())
	holders.fill("")


## Reads [param stat]'s record from a voxel state. A record only grows, so a state with a value
## no higher than the known one (an older copy, read late) is ignored. Returns whether the record
## changed.
func read(stat: Stat, state: PackedByteArray) -> bool:
	if state.size() < HEADER_SIZE or state.decode_u8(0) != FORMAT:
		return false
	if state.size() != HEADER_SIZE + state.decode_u8(5):
		return false
	var value := state.decode_float(1)
	if value <= values[stat]:
		return false
	values[stat] = value
	holders[stat] = state.slice(HEADER_SIZE).get_string_from_utf8()
	return true


## Gives [param stat]'s record to [param holder] with [param value], and returns the voxel state
## that stores it.
func set_record(stat: Stat, value: float, holder: String) -> PackedByteArray:
	values[stat] = value
	holders[stat] = holder
	var name_bytes := holder.left(MAX_NAME_LENGTH).to_utf8_buffer()
	var bytes := PackedByteArray()
	bytes.resize(HEADER_SIZE)
	bytes.encode_u8(0, FORMAT)
	bytes.encode_float(1, value)
	bytes.encode_u8(5, name_bytes.size())
	return bytes + name_bytes


## The stats in which [param stats] beat the records.
func beaten_by(stats: PlayerStats) -> Array[Stat]:
	var beaten: Array[Stat] = []
	for stat: Stat in Stat.values():
		if value_in(stats, stat) > values[stat]:
			beaten.append(stat)
	return beaten
