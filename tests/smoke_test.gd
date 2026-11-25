extends Node
## Offline smoke test of the Crowdy SDK addon: no account or server is needed. Run it after
## building the extension:
## [codeblock]
## godot --headless --path . res://tests/smoke_test.tscn
## [/codeblock]
## The exit code is the number of failed checks.

## Nothing listens here, so requests fail fast with NETWORK_ERROR.
const UNREACHABLE_API := "http://127.0.0.1:9"
const ACTOR_ID := "0123456789abcdef0123456789abcdef"

var _failures := 0
var _identity: CrowdyIdentity
var _session: CrowdyGameSession


func _process(_delta: float) -> void:
	if _identity:
		_identity.poll()
	if _session:
		_session.poll()


func _ready() -> void:
	_test_registration()
	_test_results()
	await _test_identity()
	await _test_session()
	await _test_session_config()
	print("Smoke test finished with %d failure(s)." % _failures)
	get_tree().quit(_failures)


func _check(condition: bool, description: String) -> void:
	if condition:
		print("PASS ", description)
	else:
		_failures += 1
		printerr("FAIL ", description)


func _test_registration() -> void:
	for sdk_class in ["CrowdyResult", "CrowdyRequest", "CrowdyAppToken", "CrowdyIdentity"]:
		_check(ClassDB.class_exists(sdk_class) and not ClassDB.can_instantiate(sdk_class), "%s is registered and created only by the SDK" % sdk_class)
	_check(ClassDB.can_instantiate("CrowdyGameSession"), "CrowdyGameSession can be created with new()")


func _test_results() -> void:
	var success := CrowdyResult.success({"value": 1})
	_check(success.ok and success.data.value == 1 and success.error_code.is_empty(), "CrowdyResult.success() carries its data")
	var failure := CrowdyResult.failure("SOME_CODE", "Some message")
	_check(not failure.ok and failure.error_code == "SOME_CODE" and failure.error_message == "Some message", "CrowdyResult.failure() carries its error")


func _test_identity() -> void:
	_identity = CrowdyIdentity.create(UNREACHABLE_API, false)
	_check(not _identity.is_signed_in(), "a new identity is not signed in")
	var signed_in: CrowdyResult = await _identity.sign_in("nobody@example.com", "password").completed
	_check(signed_in.error_code == "NETWORK_ERROR", "an unreachable API fails with NETWORK_ERROR (got %s)" % signed_in)
	var profile_request := _identity.fetch_profile()
	var mint_request := _identity.mint_app_token("1")
	var profile: CrowdyResult = await profile_request.completed
	var minted: CrowdyResult = await mint_request.completed
	_check(not profile.ok and not minted.ok, "concurrent requests each complete")
	_identity = null


func _test_session() -> void:
	_session = CrowdyGameSession.new()
	_check(_session.get_state() == CrowdyGameSession.STATE_STOPPED, "a new session is stopped")
	var started: CrowdyResult = await _session.start(null).completed
	_check(started.error_code == "INVALID_ARGUMENT", "start() without a token is refused (got %s)" % started)
	var reconnected: CrowdyResult = await _session.reconnect().completed
	_check(reconnected.error_code == "NOT_STARTED", "reconnect() before start() is refused (got %s)" % reconnected)
	_check(_session.join(Vector3i.ZERO, PackedByteArray([1])) == ERR_UNCONFIGURED, "join() before start() returns ERR_UNCONFIGURED")
	_check(_session.send_event(1, PackedByteArray()) == ERR_UNCONFIGURED, "send_event() before start() returns ERR_UNCONFIGURED")
	_check(_session.send_voxel_update(Vector3i.ZERO, Vector3i(-1, 0, 0), 1000, PackedByteArray([1])) == ERR_UNCONFIGURED, "send_voxel_update() before start() returns ERR_UNCONFIGURED")
	var voxels: CrowdyResult = await _session.fetch_voxels(Vector3i.ZERO, Vector3i.ONE).completed
	_check(voxels.error_code == "NOT_STARTED", "fetch_voxels() before start() is refused (got %s)" % voxels)
	_check(_session.has_signal("voxel_updated"), "the session reports voxel edits with voxel_updated")
	_check(_session.get_remote_actors().is_empty() and _session.get_actor_id().is_empty(), "no actors before start()")
	_session = null


func _test_session_config() -> void:
	var config := CrowdySessionConfig.new()
	_check(config.world_to_chunk(Vector3(17.0, -0.5, 15.9)) == Vector3i(1, -1, 0), "world_to_chunk() floors positions to chunks")
	_session = CrowdyGameSession.new()
	var unknown: CrowdyResult = await _session.start(null, {"send_rate": 20}).completed
	_check(unknown.error_code == "INVALID_ARGUMENT" and "send_rate" in unknown.error_message, "an unknown session setting is refused (got %s)" % unknown)
	config.actor_id = ACTOR_ID
	var accepted: CrowdyResult = await _session.start(null, config.to_dictionary()).completed
	_check("token" in accepted.error_message, "every CrowdySessionConfig setting is accepted (got %s)" % accepted)
	_session = null

