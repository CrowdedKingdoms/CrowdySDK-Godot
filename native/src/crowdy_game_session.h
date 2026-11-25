#pragma once

#include "async_host_election.h"
#include "background_call.h"
#include "crowdy_app_token.h"
#include "crowdy_request.h"

#include <crowdy/game_session.hpp>
#include <crowdy/session/world_session.hpp>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>

namespace crowdy_godot {

// One player's realtime presence in an app's world: the native UDP replication connection and
// CrowdyCPP's WorldSession over it. The session sends the local actor's state at a fixed rate
// (send-on-change, keyframes, idle heartbeats), tracks every remote actor in range and routes
// events and messages.
//
// The object lives as long as the game wants: start() and stop() can repeat, and connected
// signals stay connected across them. Call poll() once per frame on the main thread; every
// signal and request completion is delivered from it.
class CrowdyGameSession : public godot::RefCounted {
	GDCLASS(CrowdyGameSession, godot::RefCounted)

public:
	enum State {
		STATE_STOPPED,
		STATE_CONNECTING,
		STATE_CONNECTED,
		STATE_RECONNECTING,
		// The server released the session after the app's idle timeout. Call reconnect() on the
		// player's next input.
		STATE_INACTIVE,
		STATE_FAILED,
	};

	godot::Ref<CrowdyRequest> start(const godot::Ref<CrowdyAppToken> &p_token, const godot::Dictionary &p_config);
	godot::Ref<CrowdyRequest> reconnect();
	void stop();
	void poll();
	State get_state() const;

	godot::String get_actor_id() const;
	godot::Error join(const godot::Vector3i &p_chunk, const godot::PackedByteArray &p_state);
	bool has_joined() const;
	void set_actor_state(const godot::PackedByteArray &p_state);
	godot::Error move_to_chunk(const godot::Vector3i &p_chunk);
	godot::Array get_remote_actors() const;

	godot::Error send_event(int p_event_type, const godot::PackedByteArray &p_payload);
	godot::Error send_text(const godot::String &p_text);
	godot::Error send_channel_message(int64_t p_channel_id, const godot::PackedByteArray &p_payload);
	godot::Error send_direct_message(const godot::String &p_actor_id, const godot::Vector3i &p_chunk, const godot::PackedByteArray &p_payload);

	bool is_host() const;
	godot::String get_host_user_id() const;
	godot::Dictionary get_stats() const;

protected:
	static void _bind_methods();

private:
	// What start() reads from its config Dictionary.
	struct Settings {
		crowdy::session::WorldSessionConfig world;
		bool host_tracking = false;
		int connect_attempts = 1;
		int connect_timeout_ms = 5000;
	};

	struct StartOutcome {
		crowdy::GameSession session;
		std::string exception;
	};

	static bool parse_settings(const godot::Dictionary &p_config, Settings &r_settings, godot::String &r_error);

	void collect_start();
	void collect_reconnect();
	void create_world();
	// A reconnect() of this session is running connect() on its thread.
	bool is_reconnecting() const;
	godot::Error check_sendable() const;
	godot::Error record_send(const crowdy::Result<std::uint8_t> &p_sent, crowdy::session::SendKind p_kind);
	void update_state();

	template <typename... Args>
	void queue_signal(const char *p_signal, Args... p_args) {
		pending_signals.emplace_back([this, signal = godot::StringName(p_signal), ... args = std::move(p_args)]() {
			emit_signal(signal, args...);
		});
	}

	BackgroundCall<StartOutcome> starting;
	BackgroundCall<crowdy::Status> reconnecting;
	// 0 when no start() or reconnect() is waiting for its result; a start that arrives with
	// start_request_id 0 was stopped and is dropped.
	uint64_t start_request_id = 0;
	uint64_t reconnect_request_id = 0;
	Settings settings;

	// Destroyed in reverse order: the world before the host election and the connection it
	// uses, the host election before the client it calls.
	crowdy::GameSession game;
	std::unique_ptr<AsyncHostElection> host_election;
	std::unique_ptr<crowdy::session::WorldSession> world;

	State reported_state = STATE_STOPPED;
	// Signals raised inside SDK callbacks, emitted once the SDK has returned so that a script
	// reacting to one may call any method of this object, stop() included.
	std::deque<std::function<void()>> pending_signals;
	PendingRequests requests;
};

} // namespace crowdy_godot

VARIANT_ENUM_CAST(crowdy_godot::CrowdyGameSession::State);
