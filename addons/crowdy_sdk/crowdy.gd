extends Node
## Entry point of the Crowdy SDK, registered as the [code]Crowdy[/code] autoload.
##
## Owns the player's [CrowdyIdentity] and one [CrowdyGameSession], configured from the
## [code]crowdy/*[/code] project settings, and polls both every frame so their signals and
## request results arrive on the main thread.
## [codeblock]
## var result: CrowdyResult = await Crowdy.identity.sign_in(email, password).completed
## if result.ok:
##     result = await Crowdy.start_session()
## if not result.ok:
##     print(result.error_code, ": ", result.error_message)
## [/codeblock]

const SETTING_API_URL := "crowdy/api_url"
const SETTING_APP_ID := "crowdy/app_id"
const SETTING_PERSIST_SESSION := "crowdy/persist_session"

## The player's account: signing in, the gates a new account passes before it may play, and
## the app tokens that start sessions.
var identity := CrowdyIdentity.create(
		ProjectSettings.get_setting(SETTING_API_URL, ""),
		ProjectSettings.get_setting(SETTING_PERSIST_SESSION, true))

## The realtime session. It lives as long as the game, so signals connected to it stay
## connected across [method start_session] and [method CrowdyGameSession.stop].
var session := CrowdyGameSession.new()


func _init() -> void:
	# The session keeps the player present (heartbeats) and receives updates while the game is
	# paused; stopping the polls would let the server drop the player.
	process_mode = Node.PROCESS_MODE_ALWAYS


func _process(_delta: float) -> void:
	identity.poll()
	session.poll()


## Mints an app token for [param app_id] (the [code]crowdy/app_id[/code] project setting when
## empty) and starts [member session] with it. Await it: it returns the [CrowdyResult] of the
## step that failed, or a successful one once the session is connected.
func start_session(config: CrowdySessionConfig = null, app_id := "") -> CrowdyResult:
	if app_id.is_empty():
		app_id = ProjectSettings.get_setting(SETTING_APP_ID, "")
	if app_id.strip_edges().is_empty():
		return CrowdyResult.failure("INVALID_ARGUMENT", "Set %s in the project settings or pass an app id." % SETTING_APP_ID)

	var minted: CrowdyResult = await identity.mint_app_token(app_id).completed
	if not minted.ok:
		return minted
	var settings := config.to_dictionary() if config else {}
	return await session.start(minted.data, settings).completed
