#include "CrowdyNative.hpp"
#include "CrowdySerialization.hpp"
#include "CrowdyTransport.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include <crowdy/crowdy.hpp>
#include <crowdy/session/world_session.hpp>

#include <unordered_set>
#include <vector>
#include <sstream>

using namespace godot;

CrowdyNative::CrowdyNative() {
	crowdy::ClientConfig cfg;

	cfg.transport = crowdy::graphql::makeCurlTransport();
	cfg.asyncTransport =
		make_threaded_async_transport(cfg.transport);

	cfg.httpUrl =
		"https://api.dev.crowdedkingdoms.com/graphql";

	identityClient =
		std::make_unique<crowdy::CrowdyClient>(cfg);
}

CrowdyNative::~CrowdyNative() {
	alive_.store(false, std::memory_order_release);
	conn_generation_.fetch_add(1, std::memory_order_acq_rel);

	if (replConn) {
		replConn->disconnect();
		replConn.reset();
	}

	if (worldSession) {
		worldSession->dispose();
		worldSession.reset();
	}

	identityClient.reset();
	gameClient.reset();
}

void CrowdyNative::poll() {
	if (identityClient) {
		identityClient->poll();
	}

	if (gameClient) {
		gameClient->poll();
	}

	if (replConn) {
		if (worldSession) {
			worldSession->tick();
		}
		else {
			replConn->poll();
		}
	}

	uint64_t gen =
		conn_generation_.load(std::memory_order_acquire);

	if (!worldSession || !replication_event_cb.is_valid()) {
		return;
	}

	auto list = worldSession->actors().list();

	std::unordered_set<std::string> current;

	for (const auto* actor : list) {
		std::string hex =
			actor_uuid_to_hex(actor->uuid);

		current.insert(hex);

		auto it =
			remote_actor_last_seen_.find(hex);

		if (it == remote_actor_last_seen_.end()) {
			std::ostringstream js;

			js
				<< "{\"lifecycle\":\"joined\","
				<< "\"uuid\":\"" << hex << "\","
				<< "\"chunk\":" << chunk_to_json(actor->chunk) << ",";

			if (!actor->samples.empty()) {
				auto& sample = actor->samples.front();
				auto encoded =
					crowdy::core::base64Encode(sample.state.bytes());

				js
					<< "\"state\":\""
					<< encoded
					<< "\",";
			}
			else {
				js << "\"state\":\"\",";
			}

			js
				<< "\"lastSeenMs\":"
				<< actor->lastSeenMs
				<< ","
				<< "\"lastServerEpochMs\":"
				<< actor->lastServerEpochMs
				<< "}";

			if (
				alive_.load(std::memory_order_acquire) &&
				gen == conn_generation_.load(std::memory_order_acquire)
				) {
				call_deferred(
					"_invoke_callable",
					Callable(replication_event_cb),
					String(js.str().c_str())
				);
			}

			remote_actor_last_seen_[hex] =
				actor->lastSeenMs;
		}
		else if (it->second != actor->lastSeenMs) {
			std::ostringstream js;

			js
				<< "{\"lifecycle\":\"updated\","
				<< "\"uuid\":\"" << hex << "\","
				<< "\"chunk\":" << chunk_to_json(actor->chunk) << ",";

			if (!actor->samples.empty()) {
				auto& sample = actor->samples.front();
				auto encoded =
					crowdy::core::base64Encode(sample.state.bytes());

				js
					<< "\"state\":\""
					<< encoded
					<< "\",";
			}
			else {
				js << "\"state\":\"\",";
			}

			js
				<< "\"lastSeenMs\":"
				<< actor->lastSeenMs
				<< ","
				<< "\"lastServerEpochMs\":"
				<< actor->lastServerEpochMs
				<< "}";

			if (
				alive_.load(std::memory_order_acquire) &&
				gen == conn_generation_.load(std::memory_order_acquire)
				) {
				call_deferred(
					"_invoke_callable",
					Callable(replication_event_cb),
					String(js.str().c_str())
				);
			}

			it->second = actor->lastSeenMs;
		}
	}

	std::vector<std::string> to_remove;

	for (const auto& kv : remote_actor_last_seen_) {
		if (current.find(kv.first) == current.end()) {
			std::ostringstream js;

			js
				<< "{\"lifecycle\":\"left\","
				<< "\"uuid\":\""
				<< kv.first
				<< "\"}";

			if (
				alive_.load(std::memory_order_acquire) &&
				gen == conn_generation_.load(std::memory_order_acquire)
				) {
				call_deferred(
					"_invoke_callable",
					Callable(replication_event_cb),
					String(js.str().c_str())
				);
			}

			to_remove.push_back(kv.first);
		}
	}

	for (const auto& uuid : to_remove) {
		remote_actor_last_seen_.erase(uuid);
	}
}

void CrowdyNative::initialize(
	const String& management_url
) {
	crowdy::ClientConfig cfg;

	cfg.httpUrl =
		(std::string)management_url.utf8().ptr();

	cfg.transport =
		crowdy::graphql::makeCurlTransport();

	cfg.asyncTransport =
		make_threaded_async_transport(cfg.transport);

	identityClient =
		std::make_unique<crowdy::CrowdyClient>(cfg);
}

void CrowdyNative::initializeGameClient(
	const crowdy::domains::AppTokenResponse& token
) {
	crowdy::ClientConfig cfg;

	cfg.httpUrl =
		(std::string)token.gameApiUrl;

	cfg.discoveryUrl =
		(std::string)token.discoveryUrl;

	cfg.transport =
		crowdy::graphql::makeCurlTransport();

	cfg.asyncTransport =
		make_threaded_async_transport(cfg.transport);

	gameClient =
		std::make_unique<crowdy::CrowdyClient>(cfg);

	gameClient->setToken(token.token);
}

void CrowdyNative::_invoke_callable(
	const Callable& cb,
	const String& res
) {
	if (!alive_.load(std::memory_order_acquire)) {
		return;
	}

	if (!cb.is_valid()) {
		return;
	}

	cb.call(Variant(res));
}

void CrowdyNative::_bind_methods() {
	ClassDB::bind_method(
		D_METHOD("poll"),
		&CrowdyNative::poll
	);

	ClassDB::bind_method(
		D_METHOD("_invoke_callable", "callback", "res"),
		&CrowdyNative::_invoke_callable
	);

	ClassDB::bind_method(
		D_METHOD("initialize", "management_url"),
		&CrowdyNative::initialize
	);

	ClassDB::bind_method(
		D_METHOD("graphql_query", "endpoint", "query"),
		&CrowdyNative::graphql_query
	);

	ClassDB::bind_method(
		D_METHOD("login", "email", "password"),
		&CrowdyNative::login
	);

	ClassDB::bind_method(
		D_METHOD("login_async", "email", "password", "callback"),
		&CrowdyNative::login_async
	);

	ClassDB::bind_method(
		D_METHOD("register_user", "email", "password", "gamertag"),
		&CrowdyNative::register_user
	);

	ClassDB::bind_method(
		D_METHOD("register_user_async", "email", "password", "gamertag", "callback"),
		&CrowdyNative::register_user_async
	);

	ClassDB::bind_method(
		D_METHOD("logout"),
		&CrowdyNative::logout
	);

	ClassDB::bind_method(
		D_METHOD("logout_async", "callback"),
		&CrowdyNative::logout_async
	);

	ClassDB::bind_method(
		D_METHOD("request_login_link", "email", "redirect_uri"),
		&CrowdyNative::request_login_link
	);

	ClassDB::bind_method(
		D_METHOD("replication_set_event_callback", "callback"),
		&CrowdyNative::replication_set_event_callback
	);

	ClassDB::bind_method(
		D_METHOD("replication_set_status_callback", "callback"),
		&CrowdyNative::replication_set_status_callback
	);

	ClassDB::bind_method(
		D_METHOD("replication_clear_event_callback"),
		&CrowdyNative::replication_clear_event_callback
	);

	ClassDB::bind_method(
		D_METHOD("replication_clear_status_callback"),
		&CrowdyNative::replication_clear_status_callback
	);

	ClassDB::bind_method(
		D_METHOD("replication_connect", "app_id"),
		&CrowdyNative::replication_connect
	);

	ClassDB::bind_method(
		D_METHOD("replication_connect_async", "app_id", "callback"),
		&CrowdyNative::replication_connect_async
	);

	ClassDB::bind_method(
		D_METHOD("replication_disconnect"),
		&CrowdyNative::replication_disconnect
	);

	ClassDB::bind_method(
		D_METHOD("replication_get_connection_status"),
		&CrowdyNative::replication_get_connection_status
	);

	ClassDB::bind_method(
		D_METHOD("replication_create_world_session", "app_id"),
		&CrowdyNative::replication_create_world_session
	);

	ClassDB::bind_method(
		D_METHOD("worldsession_get_self_uuid"),
		&CrowdyNative::worldsession_get_self_uuid
	);

	ClassDB::bind_method(
		D_METHOD("worldsession_dispose"),
		&CrowdyNative::worldsession_dispose
	);

	ClassDB::bind_method(
		D_METHOD("worldsession_get_actors"),
		&CrowdyNative::worldsession_get_actors
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_actor_update",
			"uuid_hex",
			"x",
			"y",
			"z",
			"state_base64"
		),
		&CrowdyNative::replication_send_actor_update
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_actor_update_and_wait",
			"uuid_hex",
			"x",
			"y",
			"z",
			"state_base64",
			"timeout_ms"
		),
		&CrowdyNative::replication_send_actor_update_and_wait
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_actor_update_and_wait_async",
			"uuid_hex",
			"x",
			"y",
			"z",
			"state_base64",
			"timeout_ms",
			"callback"
		),
		&CrowdyNative::replication_send_actor_update_and_wait_async
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_heartbeat",
			"uuid_hex",
			"x",
			"y",
			"z"
		),
		&CrowdyNative::replication_send_heartbeat
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_channel_message",
			"channel_id",
			"uuid_hex",
			"payload_base64"
		),
		&CrowdyNative::replication_send_channel_message
	);

	ClassDB::bind_method(
		D_METHOD(
			"replication_send_single_actor_message",
			"x",
			"y",
			"z",
			"target_uuid_hex",
			"payload_base64"
		),
		&CrowdyNative::replication_send_single_actor_message
	);
}

extern "C" void initialize_crowdy_gdextension(
	ModuleInitializationLevel p_level
) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ClassDB::register_class<CrowdyNative>();
}

extern "C" void uninitialize_crowdy_gdextension(
	ModuleInitializationLevel p_level
) {
	(void)p_level;
}

extern "C" {

	GDExtensionBool GDE_EXPORT crowdy_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization* r_initialization
	) {
		godot::GDExtensionBinding::InitObject init_obj(
			p_get_proc_address,
			p_library,
			r_initialization
		);

		init_obj.register_initializer(
			initialize_crowdy_gdextension
		);

		init_obj.register_terminator(
			uninitialize_crowdy_gdextension
		);

		init_obj.set_minimum_library_initialization_level(
			MODULE_INITIALIZATION_LEVEL_SCENE
		);

		return init_obj.init();
	}

}
