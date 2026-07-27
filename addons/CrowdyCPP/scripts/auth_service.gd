extends Node

var management_url: String = "https://api.dev.crowdedkingdoms.com/graphql"
var game_url: String = "https://game.shared.dev.cks-env.com/graphql"

func _ready():
	pass

func login(email, password):
	var native = CrowdyNative.new()
	# Use dev_login for testing; replace with real login method when available
	var res = native.dev_login(management_url, email)
	print(res)

func _on_Register_pressed(email, password):
	var native = CrowdyNative.new()
	# Placeholder: CrowdyCPP registration endpoint not exposed in this wrapper yet
	print("Register not implemented")
