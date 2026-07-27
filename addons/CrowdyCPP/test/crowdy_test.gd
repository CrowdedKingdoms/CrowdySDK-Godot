extends Node

# Minimal runtime test that checks the native class registration and calls ping().
func _ready():
	print("CrowdyNative exists:", ClassDB.class_exists("CrowdyNative"))
	if ClassDB.class_exists("CrowdyNative"):
		var inst = CrowdyNative.new()
		print("ping ->", inst.ping())
	else:
		push_warning("CrowdyNative not available. Make sure the GDExtension is enabled and loaded.")
