extends Node

class_name CrowdyClient

@export var endpoint: String = ""

func _ready():
	pass

func query_graphql(query: String, variables: Dictionary = {}) -> Dictionary:
	# Placeholder: implement HTTPRequest-based GraphQL client
	return {"ok": false, "error": "not implemented"}
