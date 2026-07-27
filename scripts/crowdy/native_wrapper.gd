extends Node

class_name CrowdyNativeWrapper

@onready var _native = null

func _ready():
	# Attempt to instantiate the native class exposed by the GDExtension.
	if ClassDB.class_exists("CrowdyNative"):
		_native = CrowdyNative.new()
	else:
		push_warning("CrowdyNative class not available. Make sure the GDExtension is built and installed.")

func ping() -> String:
	if _native:
		return _native.ping()
	return "native_unavailable"

func graphql_query(endpoint: String, query: String) -> Dictionary:
	if not _native:
		return {"ok": false, "error": "native_unavailable"}
	var raw = _native.graphql_query(endpoint, query)
	# Expect the native to return a JSON string. Try parsing.
	var js = JSON.new()
	var err = js.parse(raw)
	if err != OK:
		return {"ok": false, "error": "parse_error", "text": raw}
	return {"ok": true, "result": js.get_data()}
