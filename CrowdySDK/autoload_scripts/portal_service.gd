extends Node

var native: Object = null # runtime-instantiated native wrapper

signal portal_begin_result(result)
signal portal_complete_result(result)
signal portal_mint_result(result)

func _ready():
	if native == null:
		if ClassDB.class_exists("CrowdyNative"):
			native = ClassDB.instance("CrowdyNative")
		else:
			push_warning("CrowdyNative extension not available; portal functions will be disabled.")

func portal_generate_pkce():
	var res = native.portal_generate_pkce()
	var js = JSON.new()
	var err = js.parse(res)
	if err == OK:
		return js.get_data()
	return {}

func portal_begin_entry(app_id, authorize_url, redirect_uri):
	var res = native.portal_begin_entry(app_id, authorize_url, redirect_uri)
	var js = JSON.new()
	var err = js.parse(res)
	if err == OK:
		return js.get_data()
	return {}

func portal_complete_entry_async(code, verifier):
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
		emit_signal("portal_complete_result", data)
	native.portal_complete_entry_async(code, Callable(wrapper))

func portal_mint_app_token_async(app_id):
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
		emit_signal("portal_mint_result", data)
	native.portal_mint_app_token_async(app_id, Callable(wrapper))
