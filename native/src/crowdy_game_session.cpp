#include "crowdy_game_session.h"

#include "client_config.h"
#include "conversions.h"

#include <crowdy/core/base64.hpp>
#include <crowdy/core/clock.hpp>

#include <godot_cpp/core/class_db.hpp>

#include <cstdlib>
#include <exception>
#include <limits>
#include <optional>
#include <utility>

using namespace godot;

namespace crowdy_godot {

namespace {

using crowdy::graphql::GraphQLOutcome;
using crowdy::session::SendKind;

// The most chunks one fetch_voxels() reads: the platform's limit for a box, 17 x 17 x 17.
constexpr int64_t MAX_FETCH_CHUNKS = 4913;

bool fits_int16(int64_t p_value) {
	return p_value >= std::numeric_limits<int16_t>::min() && p_value <= std::numeric_limits<int16_t>::max();
}

crowdy::domains::ChunkRef to_chunk_ref(const Vector3i &p_chunk) {
	return crowdy::domains::ChunkRef{ p_chunk.x, p_chunk.y, p_chunk.z };
}

// Chunk coordinates are BigInt strings and voxel positions numbers; asBigInt() reads either.
Vector3i vector3i_of(const crowdy::graphql::Json &p_coordinates) {
	return Vector3i(static_cast<int32_t>(p_coordinates["x"].asBigInt()), static_cast<int32_t>(p_coordinates["y"].asBigInt()),
			static_cast<int32_t>(p_coordinates["z"].asBigInt()));
}

// The edits a region read returned, one per voxel position, flattened over their chunks.
Array voxels_of(const crowdy::graphql::Json &p_region) {
	Array voxels;
	p_region["chunks"].forEach([&voxels](const crowdy::graphql::Json &p_chunk) {
		const Vector3i chunk = vector3i_of(p_chunk["coordinates"]);
		p_chunk["voxels"].forEach([&voxels, &chunk](const crowdy::graphql::Json &p_voxel) {
			PackedByteArray state;
			if (const auto decoded = crowdy::core::base64Decode(p_voxel["state"].asStringView())) {
				state = to_packed_bytes(crowdy::Bytes(decoded->data(), decoded->size()));
			}
			const std::string_view recorded_at = p_voxel["createdAt"].asStringView();
			Dictionary entry;
			entry["chunk"] = chunk;
			entry["voxel"] = vector3i_of(p_voxel["location"]);
			entry["voxel_type"] = p_voxel["voxelType"].asInt64();
			entry["state"] = state;
			entry["updated_at_ms"] = crowdy::core::parseIso8601Millis(recorded_at.data(), recorded_at.size());
			entry["user_id"] = to_godot(p_voxel["createdBy"].asBigIntString());
			voxels.push_back(entry);
		});
	});
	return voxels;
}

const char *send_kind_name(SendKind p_kind) {
	switch (p_kind) {
		case SendKind::ActorUpdate:
			return "actor_update";
		case SendKind::VoxelUpdate:
			return "voxel_update";
		case SendKind::Audio:
			return "audio";
		case SendKind::Text:
			return "text";
		case SendKind::ClientEvent:
			return "event";
		case SendKind::GenericSpatial:
			return "generic_spatial";
		case SendKind::SingleActor:
			return "direct_message";
		case SendKind::Channel:
			return "channel_message";
		case SendKind::Heartbeat:
			return "heartbeat";
		case SendKind::Unknown:
			break;
	}
	return "unknown";
}

CrowdyGameSession::State state_of(crowdy::replication::ConnState p_state) {
	using crowdy::replication::ConnState;
	switch (p_state) {
		case ConnState::Idle:
		case ConnState::Connecting:
			return CrowdyGameSession::STATE_CONNECTING;
		case ConnState::Connected:
			return CrowdyGameSession::STATE_CONNECTED;
		case ConnState::Reconnecting:
			return CrowdyGameSession::STATE_RECONNECTING;
		case ConnState::Inactive:
			return CrowdyGameSession::STATE_INACTIVE;
		case ConnState::Failed:
			return CrowdyGameSession::STATE_FAILED;
		case ConnState::Closed:
			break;
	}
	return CrowdyGameSession::STATE_STOPPED;
}

Dictionary actor_snapshot(const crowdy::session::RemoteActor &p_actor) {
	Dictionary actor;
	actor["actor_id"] = to_actor_id(p_actor.uuid);
	actor["chunk"] = to_vector3i(p_actor.chunk);
	actor["state"] = to_packed_bytes(p_actor.state());
	actor["server_time_ms"] = p_actor.lastServerEpochMs;
	return actor;
}

// Reads one integer setting within [p_min, p_max]; false (with r_error set) when it is not one.
bool read_int(const Dictionary &p_config, const char *p_key, int64_t p_min, int64_t p_max, int64_t &r_value, String &r_error) {
	if (!p_config.has(p_key)) {
		return true;
	}
	const Variant value = p_config[p_key];
	if (value.get_type() != Variant::INT || int64_t(value) < p_min || int64_t(value) > p_max) {
		r_error = vformat("Session setting \"%s\" must be an integer from %d to %d.", p_key, p_min, p_max);
		return false;
	}
	r_value = value;
	return true;
}

} // namespace

bool CrowdyGameSession::parse_settings(const Dictionary &p_config, Settings &r_settings, String &r_error) {
	static const char *const known_keys[] = {
		"send_rate_hz", "keyframe_interval_ms", "heartbeat_interval_ms", "distance", "stale_after_ms",
		"actor_id", "host_tracking", "connect_attempts", "connect_timeout_ms"
	};
	const Array keys = p_config.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		bool known = false;
		for (const char *key : known_keys) {
			known = known || keys[i] == Variant(key);
		}
		if (!known) {
			r_error = vformat("Unknown session setting \"%s\".", keys[i]);
			return false;
		}
	}

	crowdy::session::WorldSessionConfig &world = r_settings.world;
	constexpr int64_t int_max = std::numeric_limits<int>::max();
	int64_t send_rate_hz = world.self.sendHz;
	int64_t distance = world.self.distance;
	int64_t connect_attempts = r_settings.connect_attempts;
	int64_t connect_timeout_ms = r_settings.connect_timeout_ms;
	const bool valid =
			read_int(p_config, "send_rate_hz", 1, 1000, send_rate_hz, r_error) &&
			read_int(p_config, "keyframe_interval_ms", 1, int_max, world.self.keyframeIntervalMs, r_error) &&
			read_int(p_config, "heartbeat_interval_ms", 0, int_max, world.self.heartbeatIntervalMs, r_error) &&
			read_int(p_config, "distance", 0, 8, distance, r_error) &&
			read_int(p_config, "stale_after_ms", 1, int_max, world.actors.staleAfterMs, r_error) &&
			read_int(p_config, "connect_attempts", 1, 100, connect_attempts, r_error) &&
			read_int(p_config, "connect_timeout_ms", 0, int_max, connect_timeout_ms, r_error);
	if (!valid) {
		return false;
	}
	world.self.sendHz = static_cast<int>(send_rate_hz);
	world.self.distance = static_cast<uint8_t>(distance);
	r_settings.connect_attempts = static_cast<int>(connect_attempts);
	r_settings.connect_timeout_ms = static_cast<int>(connect_timeout_ms);

	if (p_config.has("actor_id")) {
		const String actor_id = p_config["actor_id"];
		crowdy::core::ActorUuid uuid;
		if (!parse_actor_id(actor_id, uuid)) {
			r_error = "Session setting \"actor_id\" must be a 32-character actor id.";
			return false;
		}
		world.actorUuid = to_std(actor_id);
	}

	r_settings.host_tracking = p_config.get("host_tracking", false);
	if (!r_settings.host_tracking) {
		world.hostHeartbeatIntervalMs = 0;
	}
	return true;
}

Ref<CrowdyRequest> CrowdyGameSession::start(const Ref<CrowdyAppToken> &p_token, const Dictionary &p_config) {
	auto [id, request] = requests.create();
	Settings parsed;
	String error;
	if (!parse_settings(p_config, parsed, error)) {
		requests.resolve(id, CrowdyResult::failure("INVALID_ARGUMENT", error));
		return request;
	}
	if (p_token.is_null()) {
		requests.resolve(id, CrowdyResult::failure("INVALID_ARGUMENT", "start() needs a token from CrowdyIdentity.mint_app_token()."));
		return request;
	}
	if (world || starting.is_running()) {
		requests.resolve(id, CrowdyResult::failure("BUSY", "The session is already started, or a stopped start() is still finishing."));
		return request;
	}
	settings = std::move(parsed);

	crowdy::GameSessionOptions options;
	options.client = make_client_config();
	options.fallbackOrigin = p_token->get_issuing_origin();
	options.connectAttempts = settings.connect_attempts;
	options.connectTimeoutMs = settings.connect_timeout_ms;

	// Connecting is blocking (a GraphQL server assignment, then the session-ready wait).
	start_request_id = id;
	starting.start([token = p_token->get_response(), options = std::move(options)]() mutable {
		try {
			return StartOutcome{ crowdy::startGameSession(token, std::move(options)), {} };
		} catch (const std::exception &e) {
			return StartOutcome{ {}, e.what() };
		}
	});
	update_state();
	return request;
}

Ref<CrowdyRequest> CrowdyGameSession::reconnect() {
	auto [id, request] = requests.create();
	if (!world) {
		requests.resolve(id, CrowdyResult::failure("NOT_STARTED", "reconnect() needs a started session."));
		return request;
	}
	if (reconnecting.is_running()) {
		requests.resolve(id, CrowdyResult::failure("BUSY", "A reconnect is already in progress."));
		return request;
	}
	const State state = get_state();
	if (state != STATE_INACTIVE && state != STATE_FAILED) {
		// Connected, or the SDK is already reassigning on its own.
		requests.resolve(id, CrowdyResult::success());
		return request;
	}
	reconnect_request_id = id;
	// The call keeps the client and the connection alive even if stop() runs meanwhile; poll()
	// does not tick the world until it returns, so the connection has one user at a time.
	reconnecting.start([session = game]() {
		try {
			return session.connection->connect();
		} catch (const std::exception &) {
			return crowdy::Status(crowdy::Errc::Rejected);
		}
	});
	return request;
}

void CrowdyGameSession::stop() {
	pending_signals.clear();
	if (start_request_id != 0) {
		requests.resolve(start_request_id, CrowdyResult::failure("CANCELLED", "The session was stopped while starting."));
		start_request_id = 0;
	}
	if (reconnect_request_id != 0) {
		requests.resolve(reconnect_request_id, CrowdyResult::failure("CANCELLED", "The session was stopped while reconnecting."));
		reconnect_request_id = 0;
	}
	for (const uint64_t id : voxel_fetch_ids) {
		requests.resolve(id, CrowdyResult::failure("CANCELLED", "The session was stopped while fetching voxels."));
	}
	voxel_fetch_ids.clear();
	if (world) {
		// Every actor_joined is matched by an actor_left, so scenes can remove what they spawned.
		world->actors().forEach([this](const crowdy::session::RemoteActor &p_actor) {
			queue_signal("actor_left", to_actor_id(p_actor.uuid));
		});
	}
	world.reset();
	host_election.reset();
	// Releasing the connection disconnects it, unless a reconnect still holds it.
	game = crowdy::GameSession();
	update_state();
}

void CrowdyGameSession::poll() {
	// A script reacting to a signal may drop its last reference to this object.
	const Ref<CrowdyGameSession> keep_alive(this);

	collect_start();
	collect_reconnect();
	if (game.client) {
		game.client->poll();
	}
	// While reconnect() runs connect() on its thread, the world must not send through the same
	// connection. A reconnect left over from a stopped session has id 0 and does not block.
	if (world && !is_reconnecting()) {
		world->tick();
	}
	update_state();

	while (!pending_signals.empty()) {
		const std::function<void()> emit = std::move(pending_signals.front());
		pending_signals.pop_front();
		emit();
	}
	requests.flush();
}

CrowdyGameSession::State CrowdyGameSession::get_state() const {
	if (start_request_id != 0) {
		return STATE_CONNECTING;
	}
	if (!world) {
		return STATE_STOPPED;
	}
	return state_of(game.connection->state());
}

void CrowdyGameSession::collect_start() {
	std::optional<StartOutcome> outcome = starting.take();
	if (!outcome) {
		return;
	}
	const uint64_t id = std::exchange(start_request_id, 0);
	if (id == 0) {
		return; // Stopped while starting; the session is released here.
	}
	if (!outcome->exception.empty()) {
		requests.resolve(id, CrowdyResult::failure("START_FAILED", to_godot(outcome->exception)));
		return;
	}
	if (!outcome->session.ok()) {
		requests.resolve(id, CrowdyResult::failure_from(outcome->session.status.code, "Could not connect to a realtime server."));
		return;
	}
	game = std::move(outcome->session);
	create_world();
	requests.resolve(id, CrowdyResult::success());
}

void CrowdyGameSession::collect_reconnect() {
	std::optional<crowdy::Status> status = reconnecting.take();
	if (!status) {
		return;
	}
	const uint64_t id = std::exchange(reconnect_request_id, 0);
	if (id == 0) {
		return;
	}
	if (!status->ok()) {
		requests.resolve(id, CrowdyResult::failure_from(status->code, "Could not reconnect to a realtime server."));
		return;
	}
	if (world->self().joined()) {
		// The server forgot the actor with the released session.
		world->self().refresh();
	}
	requests.resolve(id, CrowdyResult::success());
}

void CrowdyGameSession::create_world() {
	crowdy::session::WorldSessionConfig config = settings.world;
	config.appId = game.token.appId;
	config.onText = [this](const crowdy::replication::SpatialNotification &p_text) {
		const String text = to_godot(std::string_view(reinterpret_cast<const char *>(p_text.payload.data()), p_text.payload.size()));
		queue_signal("text_received", to_actor_id(p_text.uuid), text);
	};
	// Includes the server's echo of this player's own accepted edits.
	config.onVoxel = [this](const crowdy::replication::SpatialNotification &p_notification, const crowdy::wire::VoxelPayloadView &p_voxel) {
		queue_signal("voxel_updated", to_vector3i(p_notification.chunk), Vector3i(p_voxel.x, p_voxel.y, p_voxel.z), int64_t(p_voxel.voxelType),
				to_packed_bytes(p_voxel.state), to_actor_id(p_notification.uuid));
	};

	if (settings.host_tracking) {
		host_election = std::make_unique<AsyncHostElection>(*game.client);
	}
	// No chunk source: this wrapper does not expose the voxel world, so nothing hydrates it.
	world = std::make_unique<crowdy::session::WorldSession>(game.connection,
			crowdy::session::WorldSessionServices{ nullptr, host_election.get() }, std::move(config));

	crowdy::session::RemoteActorStore &actors = world->actors();
	actors.onJoin([this](const crowdy::session::RemoteActor &p_actor) {
		queue_signal("actor_joined", to_actor_id(p_actor.uuid), to_vector3i(p_actor.chunk), to_packed_bytes(p_actor.state()));
	});
	actors.onUpdate([this](const crowdy::session::RemoteActor &p_actor) {
		queue_signal("actor_updated", to_actor_id(p_actor.uuid), to_vector3i(p_actor.chunk), to_packed_bytes(p_actor.state()), p_actor.lastServerEpochMs);
	});
	// Fires both for a departure the server announces and for an actor that went stale.
	actors.onLeave([this](const crowdy::session::RemoteActor &p_actor) {
		queue_signal("actor_left", to_actor_id(p_actor.uuid));
	});

	world->channelInbox().onMessage([this](const crowdy::session::InboxMessage &p_message) {
		queue_signal("channel_message_received", p_message.channelId, to_actor_id(p_message.senderUuid), to_packed_bytes(p_message.payload));
	});
	// A direct message names no sender; games that need one put it in the payload.
	world->directInbox().onMessage([this](const crowdy::session::InboxMessage &p_message) {
		queue_signal("direct_message_received", to_packed_bytes(p_message.payload));
	});
	world->errors().onError([this](const crowdy::session::AttributedError &p_error) {
		queue_signal("send_rejected", String(error_code_of(p_error.code)), String(send_kind_name(p_error.kind)));
	});
	world->onHostChanged([this](const std::string &p_host_user_id) {
		queue_signal("host_changed", to_godot(p_host_user_id));
	});

	// WorldSession routes events per type only; every type goes to one signal here.
	crowdy::replication::Handlers handlers = game.connection->snapshot().handlers;
	handlers.clientEvent = [this](const crowdy::replication::SpatialNotification &p_notification, const crowdy::wire::EventPayloadView &p_event) {
		// The server also delivers a client event to its sender. Like remote actors, events are
		// reported from other actors only.
		if (p_notification.uuidArray() == world->actorUuid()) {
			return;
		}
		queue_signal("event_received", int64_t(p_event.eventType), to_actor_id(p_notification.uuid), to_packed_bytes(p_event.state), false);
	};
	handlers.serverEvent = [this](const crowdy::replication::SpatialNotification &p_notification, const crowdy::wire::EventPayloadView &p_event) {
		queue_signal("event_received", int64_t(p_event.eventType), to_actor_id(p_notification.uuid), to_packed_bytes(p_event.state), true);
	};
	game.connection->setHandlers(std::move(handlers));
}

void CrowdyGameSession::update_state() {
	const State state = get_state();
	if (state != reported_state) {
		reported_state = state;
		queue_signal("state_changed", state);
	}
}

String CrowdyGameSession::get_actor_id() const {
	return world ? to_actor_id(world->actorUuid()) : String();
}

Error CrowdyGameSession::join(const Vector3i &p_chunk, const PackedByteArray &p_state) {
	ERR_FAIL_COND_V_MSG(p_state.size() > int64_t(crowdy::session::kMaxStateBytes), ERR_INVALID_PARAMETER, "An actor state is at most 256 bytes.");
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	return to_godot_error(world->join(to_chunk(p_chunk), as_bytes(p_state)).code);
}

bool CrowdyGameSession::has_joined() const {
	return world && world->self().joined();
}

void CrowdyGameSession::set_actor_state(const PackedByteArray &p_state) {
	ERR_FAIL_COND_MSG(p_state.size() > int64_t(crowdy::session::kMaxStateBytes), "An actor state is at most 256 bytes.");
	if (world) {
		// Sent at the next send slot, and only when it changed.
		world->self().setState(as_bytes(p_state));
	}
}

Error CrowdyGameSession::move_to_chunk(const Vector3i &p_chunk) {
	if (!has_joined()) {
		return ERR_UNCONFIGURED;
	}
	const crowdy::wire::ChunkCoord chunk = to_chunk(p_chunk);
	if (chunk == world->self().chunk()) {
		return OK;
	}
	if (is_reconnecting()) {
		// Sent with the refresh that follows the reconnect.
		world->self().setChunk(chunk);
		return OK;
	}
	// Sends at once: crossing a chunk boundary should not wait for a send slot.
	return to_godot_error(world->self().moveTo(chunk).code);
}

Array CrowdyGameSession::get_remote_actors() const {
	Array actors;
	if (world) {
		world->actors().forEach([&actors](const crowdy::session::RemoteActor &p_actor) {
			actors.push_back(actor_snapshot(p_actor));
		});
	}
	return actors;
}

Error CrowdyGameSession::send_event(int p_event_type, const PackedByteArray &p_payload) {
	ERR_FAIL_COND_V_MSG(p_event_type < 0 || p_event_type > UINT16_MAX, ERR_INVALID_PARAMETER, "An event type is from 0 to 65535.");
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	const crowdy::session::LocalActorStore &self = world->self();
	return record_send(game.connection->sendClientEvent(self.chunk(), self.uuid(), static_cast<uint16_t>(p_event_type), as_bytes(p_payload), settings.world.self.distance),
			SendKind::ClientEvent);
}

Error CrowdyGameSession::send_text(const String &p_text) {
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	const std::string text = to_std(p_text);
	const crowdy::session::LocalActorStore &self = world->self();
	crowdy::replication::SpatialSend send;
	send.chunk = self.chunk();
	send.uuid = self.uuid();
	send.payload = crowdy::asBytes(text);
	send.distance = settings.world.self.distance;
	return record_send(game.connection->sendText(send), SendKind::Text);
}

Error CrowdyGameSession::send_channel_message(int64_t p_channel_id, const PackedByteArray &p_payload) {
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	return record_send(game.connection->sendChannelMessage(p_channel_id, world->actorUuid(), as_bytes(p_payload)), SendKind::Channel);
}

Error CrowdyGameSession::send_direct_message(const String &p_actor_id, const Vector3i &p_chunk, const PackedByteArray &p_payload) {
	crowdy::core::ActorUuid target;
	ERR_FAIL_COND_V_MSG(!parse_actor_id(p_actor_id, target), ERR_INVALID_PARAMETER, "An actor id is 32 characters.");
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	return record_send(game.connection->sendSingleActorMessage(to_chunk(p_chunk), target, as_bytes(p_payload)), SendKind::SingleActor);
}

Error CrowdyGameSession::send_voxel_update(const Vector3i &p_chunk, const Vector3i &p_voxel, int p_voxel_type, const PackedByteArray &p_state) {
	const bool in_range = fits_int16(p_voxel.x) && fits_int16(p_voxel.y) && fits_int16(p_voxel.z) && fits_int16(p_voxel_type);
	ERR_FAIL_COND_V_MSG(!in_range, ERR_INVALID_PARAMETER, "Voxel positions and types are from -32768 to 32767.");
	ERR_FAIL_COND_V_MSG(p_state.size() > int64_t(crowdy::wire::voxel::kMaxStateSize), ERR_INVALID_PARAMETER, "A voxel state is at most 1024 bytes.");
	const Error sendable = check_sendable();
	if (sendable != OK) {
		return sendable;
	}
	const crowdy::session::LocalActorStore &self = world->self();
	return record_send(game.connection->sendVoxelUpdate(to_chunk(p_chunk), self.uuid(), static_cast<int16_t>(p_voxel.x), static_cast<int16_t>(p_voxel.y),
							   static_cast<int16_t>(p_voxel.z), static_cast<int16_t>(p_voxel_type), as_bytes(p_state), settings.world.self.distance),
			SendKind::VoxelUpdate);
}

Ref<CrowdyRequest> CrowdyGameSession::fetch_voxels(const Vector3i &p_low_chunk, const Vector3i &p_high_chunk) {
	auto [id, request] = requests.create();
	if (!world) {
		requests.resolve(id, CrowdyResult::failure("NOT_STARTED", "fetch_voxels() needs a started session."));
		return request;
	}
	int64_t chunk_count = 1;
	for (int axis = 0; axis < 3; axis++) {
		chunk_count *= std::llabs(int64_t(p_high_chunk[axis]) - int64_t(p_low_chunk[axis])) + 1;
		if (chunk_count > MAX_FETCH_CHUNKS) {
			requests.resolve(id, CrowdyResult::failure("INVALID_ARGUMENT", "fetch_voxels() reads at most 4913 chunks, a box of 17 x 17 x 17."));
			return request;
		}
	}
	// One page holding every chunk of the box.
	crowdy::domains::ChunkRegion box = crowdy::domains::ChunkRegion::box(to_chunk_ref(p_low_chunk), to_chunk_ref(p_high_chunk));
	box.limit(chunk_count);
	voxel_fetch_ids.insert(id);
	game.client->voxels().listInRegionAsync(game.token.appId, box, [this, id](GraphQLOutcome p_outcome) {
		if (voxel_fetch_ids.erase(id) == 0) {
			return;
		}
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success(voxels_of(p_outcome.data)) : CrowdyResult::failure_from(p_outcome));
	});
	return request;
}

bool CrowdyGameSession::is_reconnecting() const {
	return reconnect_request_id != 0;
}

Error CrowdyGameSession::check_sendable() const {
	if (!world) {
		return ERR_UNCONFIGURED;
	}
	// The socket is not to be used from here while reconnect() reopens it on its own thread.
	return is_reconnecting() ? ERR_BUSY : OK;
}

Error CrowdyGameSession::record_send(const crowdy::Result<std::uint8_t> &p_sent, SendKind p_kind) {
	if (!p_sent.ok()) {
		return to_godot_error(p_sent.error());
	}
	// Lets send_rejected name what a server error frame refused.
	world->errors().recordSend(p_sent.value(), p_kind, world->actorUuid());
	return OK;
}

bool CrowdyGameSession::is_host() const {
	return world && world->amIHost();
}

String CrowdyGameSession::get_host_user_id() const {
	return world ? to_godot(world->hostUserId()) : String();
}

Dictionary CrowdyGameSession::get_stats() const {
	Dictionary stats;
	if (!world) {
		return stats;
	}
	// Local diagnostics, not billing figures: see crowdy::replication::Connection::Stats.
	const crowdy::replication::Connection::Stats counters = game.connection->stats();
	stats["messages_sent"] = int64_t(counters.messagesSent);
	stats["messages_received"] = int64_t(counters.messagesReceived);
	stats["bytes_sent"] = int64_t(counters.bytesSent);
	stats["bytes_received"] = int64_t(counters.bytesReceived);
	stats["sends_deferred"] = int64_t(counters.sendsDeferred);
	stats["sends_failed"] = int64_t(counters.sendsFailed);
	stats["reconnects"] = int64_t(counters.reconnects);
	stats["remote_actors"] = int64_t(world->actors().size());
	return stats;
}

void CrowdyGameSession::_bind_methods() {
	ClassDB::bind_method(D_METHOD("start", "token", "config"), &CrowdyGameSession::start, DEFVAL(Dictionary()));
	ClassDB::bind_method(D_METHOD("reconnect"), &CrowdyGameSession::reconnect);
	ClassDB::bind_method(D_METHOD("stop"), &CrowdyGameSession::stop);
	ClassDB::bind_method(D_METHOD("poll"), &CrowdyGameSession::poll);
	ClassDB::bind_method(D_METHOD("get_state"), &CrowdyGameSession::get_state);

	ClassDB::bind_method(D_METHOD("get_actor_id"), &CrowdyGameSession::get_actor_id);
	ClassDB::bind_method(D_METHOD("join", "chunk", "state"), &CrowdyGameSession::join);
	ClassDB::bind_method(D_METHOD("has_joined"), &CrowdyGameSession::has_joined);
	ClassDB::bind_method(D_METHOD("set_actor_state", "state"), &CrowdyGameSession::set_actor_state);
	ClassDB::bind_method(D_METHOD("move_to_chunk", "chunk"), &CrowdyGameSession::move_to_chunk);
	ClassDB::bind_method(D_METHOD("get_remote_actors"), &CrowdyGameSession::get_remote_actors);

	ClassDB::bind_method(D_METHOD("send_event", "event_type", "payload"), &CrowdyGameSession::send_event, DEFVAL(PackedByteArray()));
	ClassDB::bind_method(D_METHOD("send_text", "text"), &CrowdyGameSession::send_text);
	ClassDB::bind_method(D_METHOD("send_channel_message", "channel_id", "payload"), &CrowdyGameSession::send_channel_message);
	ClassDB::bind_method(D_METHOD("send_direct_message", "actor_id", "chunk", "payload"), &CrowdyGameSession::send_direct_message);

	ClassDB::bind_method(D_METHOD("send_voxel_update", "chunk", "voxel", "voxel_type", "state"), &CrowdyGameSession::send_voxel_update, DEFVAL(PackedByteArray()));
	ClassDB::bind_method(D_METHOD("fetch_voxels", "low_chunk", "high_chunk"), &CrowdyGameSession::fetch_voxels);

	ClassDB::bind_method(D_METHOD("is_host"), &CrowdyGameSession::is_host);
	ClassDB::bind_method(D_METHOD("get_host_user_id"), &CrowdyGameSession::get_host_user_id);
	ClassDB::bind_method(D_METHOD("get_stats"), &CrowdyGameSession::get_stats);

	BIND_ENUM_CONSTANT(STATE_STOPPED);
	BIND_ENUM_CONSTANT(STATE_CONNECTING);
	BIND_ENUM_CONSTANT(STATE_CONNECTED);
	BIND_ENUM_CONSTANT(STATE_RECONNECTING);
	BIND_ENUM_CONSTANT(STATE_INACTIVE);
	BIND_ENUM_CONSTANT(STATE_FAILED);

	const PropertyInfo actor_id(Variant::STRING, "actor_id");
	const PropertyInfo chunk(Variant::VECTOR3I, "chunk");
	const PropertyInfo payload(Variant::PACKED_BYTE_ARRAY, "payload");
	ADD_SIGNAL(MethodInfo("state_changed", PropertyInfo(Variant::INT, "state", PROPERTY_HINT_ENUM, "Stopped,Connecting,Connected,Reconnecting,Inactive,Failed", PROPERTY_USAGE_CLASS_IS_ENUM, "CrowdyGameSession.State")));
	ADD_SIGNAL(MethodInfo("actor_joined", actor_id, chunk, PropertyInfo(Variant::PACKED_BYTE_ARRAY, "state")));
	ADD_SIGNAL(MethodInfo("actor_updated", actor_id, chunk, PropertyInfo(Variant::PACKED_BYTE_ARRAY, "state"), PropertyInfo(Variant::INT, "server_time_ms")));
	ADD_SIGNAL(MethodInfo("actor_left", actor_id));
	ADD_SIGNAL(MethodInfo("event_received", PropertyInfo(Variant::INT, "event_type"), PropertyInfo(Variant::STRING, "sender_id"), payload, PropertyInfo(Variant::BOOL, "from_server")));
	ADD_SIGNAL(MethodInfo("text_received", PropertyInfo(Variant::STRING, "sender_id"), PropertyInfo(Variant::STRING, "text")));
	ADD_SIGNAL(MethodInfo("channel_message_received", PropertyInfo(Variant::INT, "channel_id"), PropertyInfo(Variant::STRING, "sender_id"), payload));
	ADD_SIGNAL(MethodInfo("direct_message_received", payload));
	ADD_SIGNAL(MethodInfo("voxel_updated", chunk, PropertyInfo(Variant::VECTOR3I, "voxel"), PropertyInfo(Variant::INT, "voxel_type"),
			PropertyInfo(Variant::PACKED_BYTE_ARRAY, "state"), PropertyInfo(Variant::STRING, "sender_id")));
	ADD_SIGNAL(MethodInfo("host_changed", PropertyInfo(Variant::STRING, "host_user_id")));
	ADD_SIGNAL(MethodInfo("send_rejected", PropertyInfo(Variant::STRING, "error_code"), PropertyInfo(Variant::STRING, "send_kind")));
}

} // namespace crowdy_godot
