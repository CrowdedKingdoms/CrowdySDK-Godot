extends Node

# ReplicationService: high-level Godot wrapper for CrowdyCPP replication.
# Mirrors the style of addons/CrowdyCPP/scripts/auth_service.gd

var native = null

signal replication_connected()
signal replication_disconnected()
signal replication_error(err)
signal player_updated(players)
signal players_updated(players)
signal player_joined(player)
signal player_left(player)
signal spatial_notification(notification)
signal replication_status_changed(status)

var _players := {}

func _ready():
	if AuthService and AuthService.native:
		native = AuthService.native
		native.initialize(Config.game_api_url)
	# register native callbacks
	# Prefer lifecycle callbacks from the native SDK when available. The
	# native wrapper forwards spatial notifications via replication_set_event_callback.
	native.replication_set_event_callback(Callable(self, "_on_native_event"))
	native.replication_set_status_callback(Callable(self, "_on_native_status"))

	# Start replication after auth success. AuthService emits login_completed.
	if not AuthService.login_completed.is_connected(_on_login_completed):
		AuthService.login_completed.connect(_on_login_completed)
	
	print("ready")
	# Register high-level actor lifecycle handlers if WorldSession exposes them
	# (the native wrapper will forward actorAdded/actorRemoved/actorUpdated via
	# the same event callback with a "lifecycle" field). We handle them in
	# _on_native_event and emit player_joined/player_left/player_updated.

func _on_login_completed(res):
	print("replication _on_login_completed")
	if typeof(res) == TYPE_DICTIONARY and res.has("ok") and res.ok:
		ReplicationService.start_replication_async(Config.app_id)

func _process(delta: float) -> void:
	native.poll()
	
	if Engine.get_process_frames() % 120 == 0:
		#print(replication_get_status())
		pass

# High-level helpers -------------------------------------------------------
func start_replication_async(app_id: String):
	print("start_replication_async")
	# connect async; connect callback receives JSON string
	var cb = func(raw):
		print("CONNECT RESULT:", raw)
		var js = JSON.new()
		var err = js.parse(raw)
		if err != OK:
			emit_signal("replication_error", {"error":"invalid response"})
			print("REPLICATION ERROR")
			return
		var data = js.get_data()
		if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
			# create a world session for higher-level actor stores
			var res = native.replication_create_world_session(app_id)
			print("replication_create_world_session result:", res)
			emit_signal("replication_connected")
			# emit current player list once
			_emit_players_snapshot()
		else:
			emit_signal("replication_error", data)

	native.replication_connect_async(app_id, Callable(cb))
	print("start_replication_async done")

func get_self_uuid() -> String:
	# Return local actor uuid as hex string from the native WorldSession, or
	# empty string if no WorldSession exists yet.
	return native.worldsession_get_self_uuid()

func stop_replication():
	native.replication_disconnect()
	native.worldsession_dispose()
	emit_signal("replication_disconnected")

# Low-level access ---------------------------------------------------------
func replication_get_status():
	var res = native.replication_get_connection_status()
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return {}

func get_players():
	# returns array of dictionaries
	var res = native.worldsession_get_actors()
	print("get players: ", res)
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return []

# Sending helpers ---------------------------------------------------------
func send_actor_update(uuid_hex: String, x: int, y: int, z: int, state_base64: String) -> Dictionary:
	var res = native.replication_send_actor_update(uuid_hex, x, y, z, state_base64)
	print("send_actor_update: ", res)
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return {"ok":false, "error":"invalid response"}

func send_actor_update_and_wait_async(uuid_hex: String, x: int, y: int, z: int, state_base64: String, timeout_ms: int, callback: Callable):
	# callback will be invoked with a dictionary result
	var wrapper = func(raw):
		var js = JSON.new()
		if js.parse(raw) == OK:
			callback.call(js.get_data())
		else:
			callback.call({"ok":false, "error":"invalid response"})

	native.replication_send_actor_update_and_wait_async(uuid_hex, x, y, z, state_base64, timeout_ms, Callable(wrapper))

func send_heartbeat(uuid_hex: String, x: int, y: int, z: int) -> Dictionary:
	var res = native.replication_send_heartbeat(uuid_hex, x, y, z)
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return {"ok":false, "error":"invalid response"}

func send_channel_message(channel_id: int, uuid_hex: String, payload_base64: String) -> Dictionary:
	var res = native.replication_send_channel_message(channel_id, uuid_hex, payload_base64)
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return {"ok":false, "error":"invalid response"}

func send_single_actor_message(x: int, y: int, z: int, target_uuid_hex: String, payload_base64: String) -> Dictionary:
	var res = native.replication_send_single_actor_message(x, y, z, target_uuid_hex, payload_base64)
	var js = JSON.new()
	if js.parse(res) == OK:
		return js.get_data()
	return {"ok":false, "error":"invalid response"}

# Internal helpers --------------------------------------------------------
func _on_native_event(raw):
	print("_on_native_event")
	var js = JSON.new()
	if js.parse(raw) != OK:
		return
	var data = js.get_data()
	# If this event contains an explicit lifecycle hint, prefer that for
	# player join/leave/update signals. Otherwise treat it as a generic
	# spatial notification.
	if typeof(data) == TYPE_DICTIONARY and data.has("lifecycle"):
		var typ = data.lifecycle
		if typ == "joined":
			emit_signal("player_joined", data)
			# keep snapshot in sync
			_emit_players_snapshot()
		elif typ == "left":
			emit_signal("player_left", data)
			_emit_players_snapshot()
		elif typ == "updated":
			emit_signal("player_updated", data)
			_emit_players_snapshot()
		else:
			emit_signal("spatial_notification", data)
	else:
		emit_signal("spatial_notification", data)
		# Update players snapshot when spatial notifications arrive
		_emit_players_snapshot()

func _on_native_status(raw):
	print("_on_native_status")
	var js = JSON.new()
	if js.parse(raw) != OK:
		return
	var data = js.get_data()
	emit_signal("replication_status_changed", data)

func _emit_players_snapshot():
	print("_emit_players_snapshot")
	var arr = get_players()
	_players.clear()
	for p in arr:
		if typeof(p) == TYPE_DICTIONARY and p.has("uuid"):
			_players[p.uuid] = p
	emit_signal("players_updated", arr)

# Utility: allow GDScript to register/unregister low-level callbacks
func set_native_event_callback(cb: Callable):
	native.replication_set_event_callback(cb)

func clear_native_event_callback():
	native.replication_clear_event_callback()

func set_native_status_callback(cb: Callable):
	native.replication_set_status_callback(cb)

func clear_native_status_callback():
	native.replication_clear_status_callback()
