class_name DemoInfo
extends Resource
## One demo as the main menu lists it: what to call it, the scenes it plays in, and the session
## settings it plays online with.

## The name shown in the main menu.
@export var title := ""
## The scene the demo plays in, loaded once the player picks it.
@export_file("*.tscn") var world_scene := ""
## Where the demo starts online, after sign-in, such as a lobby. Empty starts world_scene.
@export_file("*.tscn") var online_scene := ""
## The settings its Crowdy session starts with when played online.
@export var session_config: CrowdySessionConfig
