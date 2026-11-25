class_name ArenaHud
extends CanvasLayer
## The arena's on-screen text: this player's stats, the mode and who is on the platform, a banner
## for what matters right now, and a feed of falls, wins and arrivals.

## The feed shows at most FEED_LENGTH lines, each for FEED_LINE_TIME seconds before it fades.
const FEED_LENGTH := 6
const FEED_LINE_TIME := 8.0

## How feed lines look.
@export var feed_line_settings: LabelSettings

@onready var stats_label: Label = %StatsLabel
@onready var arena_label: Label = %ArenaLabel
@onready var banner_label: Label = %BannerLabel
@onready var feed: VBoxContainer = %Feed


## Minutes and seconds, as 1:05.
static func _clock(seconds: float) -> String:
	return "%d:%02d" % [floori(seconds / 60.0), floori(seconds) % 60]


func show_stats(stats: PlayerStats) -> void:
	stats_label.text = "\n".join([
		"On the platform\t%s" % _clock(stats.time_on_platform),
		"Alone on it\t%s" % _clock(stats.time_alone),
		"Players pushed off\t%d" % stats.players_pushed_off,
		"Falls\t%d" % stats.falls,
		"Best run\t%s" % _clock(stats.best_run),
		"Rounds won\t%d" % stats.rounds_won,
	])


## Shows the mode, the round and how many of [param player_count] players are on the platform.
## [param can_switch_mode] adds how to switch the mode.
func show_arena(state: ArenaState, players_on_platform: int, player_count: int, can_switch_mode: bool) -> void:
	var lines := PackedStringArray()
	if state.mode == ArenaState.Mode.ROUNDS:
		lines.append("Rounds: round %d" % state.round_number)
	else:
		lines.append("Endless")
	lines.append("%d of %d on the platform" % [players_on_platform, player_count])
	if can_switch_mode:
		lines.append("M switches the mode")
	arena_label.text = "\n".join(lines)


func show_banner(text: String) -> void:
	banner_label.text = text


func add_feed_line(text: String) -> void:
	var line := Label.new()
	line.text = text
	line.label_settings = feed_line_settings
	feed.add_child(line)
	if feed.get_child_count() > FEED_LENGTH:
		var oldest := feed.get_child(0)
		feed.remove_child(oldest)
		oldest.queue_free()
	var fade := line.create_tween()
	fade.tween_interval(FEED_LINE_TIME)
	fade.tween_property(line, ^"modulate:a", 0.0, 1.0)
	fade.tween_callback(line.queue_free)
