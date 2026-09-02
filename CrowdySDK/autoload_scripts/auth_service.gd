extends Node


var session_token: String = ""
var current_user: Dictionary = {}
var native = CrowdyNative.new()

signal login_completed(result)
signal register_completed(result)
signal logout_completed(result)
signal dev_login_completed(result)

@export var poll_rate: float = 30.0
var poll_timer := 0.0


func _ready():
	pass

func _process(delta: float) -> void:
	poll_timer += delta
		
	var interval := 1.0 / poll_rate
	
	while poll_timer >= interval:
		poll_timer -= interval
		native.poll()

func dev_login(email):
	var res = native.dev_login(email)
	var js = JSON.new()
	var err = js.parse(res)
	var data = {}
	if err == OK:
		data = js.get_data()
		if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
			session_token = data.token
			current_user = data.user
	return res
	
func dev_login_async(email):
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
			if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
				session_token = data.token
				current_user = data.user
		emit_signal("dev_login_completed", data)
		
	native.dev_login_async(email, Callable(wrapper))

func login(email, password):
	var res = native.login(email, password)
	var js = JSON.new()
	var err = js.parse(res)
	var data = {}
	if err == OK:
		data = js.get_data()
		if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
			session_token = data.token
			current_user = data.user
	return res

func login_async(email, password, user_cb = null):
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
			if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
				session_token = data.token
				current_user = data.user
		emit_signal("login_completed", data)
		
	native.login_async(email, password, Callable(wrapper))

func register(email, password, gamertag = ""):
	var res = native.register_user(email, password, gamertag)
	var js = JSON.new()
	var err = js.parse(res)
	var data = {}
	if err == OK:
		data = js.get_data()
		if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
			session_token = data.token
			current_user = data.user
	return res

func register_async(email, password, gamertag = ""):
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
			if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
				session_token = data.token
				current_user = data.user
		emit_signal("register_completed", data)
	native.register_user_async(email, password, gamertag, Callable(wrapper))

func logout():
	var res = native.logout()
	var js = JSON.new()
	var err = js.parse(res)
	var data = {}
	if err == OK:
		data = js.get_data()
		if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
			session_token = ""
			current_user = {}
	return res

func logout_async():
	var wrapper = func(raw):
		var js = JSON.new()
		var err = js.parse(raw)
		var data = {}
		if err == OK:
			data = js.get_data()
			if typeof(data) == TYPE_DICTIONARY and data.has("ok") and data.ok:
				session_token = ""
				current_user = {}
		emit_signal("logout_completed", data)
	native.logout_async(Callable(wrapper))
