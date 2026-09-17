#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/string.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <crowdy/core/uuid.hpp>

namespace crowdy {
	class CrowdyClient;

	namespace domains {
		struct AppTokenResponse;
	}

	namespace replication {
		class Connection;
	}

	namespace session {
		class WorldSession;
	}
}

class CrowdyNative : public godot::RefCounted {
	GDCLASS(CrowdyNative, godot::RefCounted)

private:
	std::unique_ptr<crowdy::CrowdyClient> identityClient;
	std::unique_ptr<crowdy::CrowdyClient> gameClient;

	std::shared_ptr<crowdy::replication::Connection> replConn;
	std::unique_ptr<crowdy::session::WorldSession> worldSession;

	godot::Callable replication_event_cb;
	godot::Callable replication_status_cb;

	std::atomic_uint64_t conn_generation_{ 0 };
	std::atomic_bool alive_{ true };

	std::unordered_map<std::string, std::int64_t> remote_actor_last_seen_;

	static void _bind_methods();

	void _invoke_callable(
		const godot::Callable& cb,
		const godot::String& res
	);

public:
	CrowdyNative();
	~CrowdyNative() override;

	// ---------------------------------------------------------------------
	// Core
	// ---------------------------------------------------------------------

	void poll();

	void initialize(const godot::String& management_url);

	void initializeGameClient(
		const crowdy::domains::AppTokenResponse& token
	);

	// ---------------------------------------------------------------------
	// Authentication
	// ---------------------------------------------------------------------

	godot::String login(
		const godot::String& email,
		const godot::String& password
	);

	void login_async(
		const godot::String& email,
		const godot::String& password,
		const godot::Callable& cb
	);

	godot::String register_user(
		const godot::String& email,
		const godot::String& password,
		const godot::String& gamertag
	);

	void register_user_async(
		const godot::String& email,
		const godot::String& password,
		const godot::String& gamertag,
		const godot::Callable& cb
	);

	godot::String logout();

	void logout_async(
		const godot::Callable& cb
	);

	godot::String graphql_query(
		const godot::String& endpoint,
		const godot::String& query
	);

	godot::String request_login_link(
		const godot::String& email,
		const godot::String& redirect_uri
	);

	// ---------------------------------------------------------------------
	// Replication
	// ---------------------------------------------------------------------

	void replication_set_event_callback(
		const godot::Callable& cb
	);

	void replication_set_status_callback(
		const godot::Callable& cb
	);

	void replication_clear_event_callback();
	void replication_clear_status_callback();

	godot::String replication_connect(
		const godot::String& app_id
	);

	void replication_connect_async(
		const godot::String& app_id,
		const godot::Callable& connect_cb
	);

	void replication_disconnect();

	godot::String replication_get_connection_status();

	godot::String replication_create_world_session(
		const godot::String& app_id
	);

	godot::String worldsession_get_self_uuid();

	void worldsession_dispose();

	godot::String worldsession_get_actors();

	godot::String replication_send_actor_update(
		const godot::String& uuid_hex,
		int x,
		int y,
		int z,
		const godot::String& state_base64
	);

	godot::String replication_send_actor_update_and_wait(
		const godot::String& uuid_hex,
		int x,
		int y,
		int z,
		const godot::String& state_base64,
		int timeout_ms
	);

	void replication_send_actor_update_and_wait_async(
		const godot::String& uuid_hex,
		int x,
		int y,
		int z,
		const godot::String& state_base64,
		int timeout_ms,
		const godot::Callable& cb
	);

	godot::String replication_send_heartbeat(
		const godot::String& uuid_hex,
		int x,
		int y,
		int z
	);

	godot::String replication_send_channel_message(
		int64_t channel_id,
		const godot::String& uuid_hex,
		const godot::String& payload_base64
	);

	godot::String replication_send_single_actor_message(
		int x,
		int y,
		int z,
		const godot::String& target_uuid_hex,
		const godot::String& payload_base64
	);

	// ---------------------------------------------------------------------
	// Internal helpers
	// ---------------------------------------------------------------------

	static bool actor_uuid_from_hex(
		const std::string& hex,
		crowdy::core::ActorUuid& out
	);
};
