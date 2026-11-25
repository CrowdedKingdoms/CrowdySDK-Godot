class_name MatchDirectory
extends RefCounted
## Where Push Arena's matches are in the Crowdy world, and how the lobby finds them. Every arena
## is one Crowdy chunk: the public arena, a row of public match slots beside it, and private
## matches at random chunks far away. All of them are at least 2 km from the shared_world demo's
## players, who play the same Crowdy app. Needs a started session.

## The public arena: always Endless, and not listed.
const PUBLIC_ARENA_CHUNK := Vector3i(128, 0, 0)
## Public matches take one of these slots, in a row along +X from the public arena. The slots are
## 16 chunks apart, beyond the 8-chunk replication range, so neighbouring matches never meet.
const PUBLIC_SLOT_COUNT := 16
const SLOT_SPACING := 16
## Private matches go to a random chunk this far out on X and on Z, on either side.
const PRIVATE_MIN_DISTANCE := 100000
const PRIVATE_MAX_DISTANCE := 1000000
## Random private chunks tried before giving up, in case one holds a live match.
const PRIVATE_ATTEMPTS := 3


static func public_slot(index: int) -> Vector3i:
	return PUBLIC_ARENA_CHUNK + Vector3i(SLOT_SPACING * (index + 1), 0, 0)


## The live public matches. Await it: the result's data is an Array of MatchConfig.
static func list_public_matches() -> CrowdyResult:
	var low := public_slot(0)
	var high := public_slot(PUBLIC_SLOT_COUNT - 1)
	var fetched: CrowdyResult = await Crowdy.session.fetch_voxels(low, high).completed
	if not fetched.ok:
		return fetched
	var matches: Array[MatchConfig] = []
	for entry: Dictionary in fetched.data:
		var config := MatchConfig.from_voxel(entry)
		if config and config.is_live():
			matches.append(config)
	return CrowdyResult.success(matches)


## A public slot with no live match in it, picked at random. Await it: the result's data is the
## slot's chunk.
static func find_free_public_slot() -> CrowdyResult:
	var listed := await list_public_matches()
	if not listed.ok:
		return listed
	var free_slots: Array[Vector3i] = []
	for index in PUBLIC_SLOT_COUNT:
		free_slots.append(public_slot(index))
	for config: MatchConfig in listed.data:
		free_slots.erase(config.chunk)
	if free_slots.is_empty():
		return CrowdyResult.failure("NO_FREE_SLOT", "All %d public match slots are in use." % PUBLIC_SLOT_COUNT)
	return CrowdyResult.success(free_slots.pick_random())


## A random far chunk with no live match in it. Await it: the result's data is the chunk.
static func find_free_private_chunk() -> CrowdyResult:
	for attempt in PRIVATE_ATTEMPTS:
		var chunk := Vector3i(_random_far_coordinate(), 0, _random_far_coordinate())
		var read := await read_match(chunk)
		if not read.ok:
			return read
		if read.data == null or not read.data.is_live():
			return CrowdyResult.success(chunk)
	return CrowdyResult.failure("NO_FREE_CHUNK", "Every chunk tried holds a live match. Try again.")


## The match in [param chunk]. Await it: the result's data is its MatchConfig, or null when the
## chunk holds no match.
static func read_match(chunk: Vector3i) -> CrowdyResult:
	var fetched: CrowdyResult = await Crowdy.session.fetch_voxels(chunk, chunk).completed
	if not fetched.ok:
		return fetched
	for entry: Dictionary in fetched.data:
		var config := MatchConfig.from_voxel(entry)
		if config:
			return CrowdyResult.success(config)
	return CrowdyResult.success(null)


static func _random_far_coordinate() -> int:
	var distance := randi_range(PRIVATE_MIN_DISTANCE, PRIVATE_MAX_DISTANCE)
	return distance if randf() < 0.5 else -distance
