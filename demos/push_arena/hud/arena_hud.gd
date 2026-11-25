class_name ArenaHud
extends CanvasLayer
## The arena's on-screen text: this player's stats, the arena's records, where the arena is and
## who is on the platform, a banner for what matters right now, and a feed of falls, wins,
## records and arrivals.

## The feed shows at most FEED_LENGTH lines, each for FEED_LINE_TIME seconds before it fades.
const FEED_LENGTH := 6
const FEED_LINE_TIME := 8.0

## How feed lines look.
@export var feed_line_settings: LabelSettings

@onready var stats_label: Label = %StatsLabel
@onready var records_panel: PanelContainer = %RecordsPanel
@onready var records_label: Label = %RecordsLabel
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


## Shows the records panel, which stays hidden offline.
func show_records(records: ArenaRecords) -> void:
	var lines := PackedStringArray(["Records"])
	for stat: ArenaRecords.Stat in ArenaRecords.Stat.values():
		var holder := records.holders[stat]
		var value := records.values[stat]
		var timed := stat == ArenaRecords.Stat.LONGEST_RUN or stat == ArenaRecords.Stat.TIME_ALONE
		var shown := _clock(value) if timed else str(roundi(value))
		lines.append("%s\t%s" % [ArenaRecords.NAMES[stat], "-" if holder.is_empty() else "%s  %s" % [shown, holder]])
	records_label.text = "\n".join(lines)
	records_panel.show()


## Shows where the arena is ([param location]), its mode and round, and how many of
## [param player_count] players are on the platform.
func show_arena(mode: MatchConfig.Mode, state: ArenaState, players_on_platform: int, player_count: int, location: String) -> void:
	var rules := "Rounds: round %d" % state.round_number if mode == MatchConfig.Mode.ROUNDS else "Endless"
	arena_label.text = "%s\n%s\n%d of %d on the platform" % [location, rules, players_on_platform, player_count]


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
