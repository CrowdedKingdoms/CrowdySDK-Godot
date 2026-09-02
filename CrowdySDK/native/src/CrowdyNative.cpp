#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/core/error_macros.hpp>

#include <vector>
#include <cstdint>
#include <mutex>
#include <string>
#include <sstream>
#include <memory>
#include <thread>
#include <iomanip>
#include <atomic>
#include <unordered_map>
#include <unordered_set>

#include <crowdy/crowdy.hpp>
#include <gdextension_interface.h>

using namespace godot;
namespace graphql = crowdy::graphql;

#include <crowdy/replication/connection.hpp>
#include <crowdy/session/world_session.hpp>

// Simple threaded async transport for engines that don't provide one.
// Runs the synchronous transport on a background thread and invokes the
// completion callback when done. Callbacks may run on that background
// thread; GraphQLClient will route them through its Dispatcher so they
// execute on the game thread when poll() is called.
namespace {
	class ThreadedAsyncTransport final : public crowdy::graphql::IAsyncHttpTransport {
	public:
		explicit ThreadedAsyncTransport(std::shared_ptr<crowdy::graphql::IHttpTransport> sync)
			: sync_(std::move(sync)) {
		}

		void sendAsync(const crowdy::graphql::HttpRequest& request,
			std::function<void(crowdy::graphql::HttpOutcome)> cb) override {
			auto sync = sync_;
			// Launch a detached thread to avoid blocking the caller (Godot main
			// thread). The callback is invoked on the worker thread; the
			// GraphQLClient Dispatcher will move delivery to the poll() caller.
			std::thread([sync, request, cb = std::move(cb)]() mutable {
				crowdy::graphql::HttpOutcome out;
#ifndef CROWDY_NO_EXCEPTIONS
				try {
					out.response = sync->send(request);
					out.status = crowdy::Errc::Ok;
				}
				catch (const crowdy::graphql::CrowdyTimeoutError& e) {
					out.status = crowdy::Errc::Timeout;
					out.errorMessage = e.what();
				}
				catch (const std::exception& e) {
					out.status = crowdy::Errc::SocketError;
					out.errorMessage = e.what();
				}
#else
				out.response = sync->send(request);
				out.status = crowdy::Errc::Ok;
#endif
				cb(std::move(out));
				}).detach();
		}

	private:
		std::shared_ptr<crowdy::graphql::IHttpTransport> sync_;
	};
} // namespace

// Helpers: marshal replication types into JSON for Godot callbacks.
static std::string actor_uuid_to_hex(const crowdy::core::ActorUuid& u) {
  const unsigned char* data = reinterpret_cast<const unsigned char*>(u.data());
  std::ostringstream o;
  for (size_t i = 0; i < crowdy::wire::kUuidSize; ++i) {
	o << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
  }
  return o.str();
}

static std::string chunk_to_json(const crowdy::wire::ChunkCoord& c) {
  std::ostringstream o;
  o << "{\"x\":" << c.x << ",\"y\":" << c.y << ",\"z\":" << c.z << "}";
  return o.str();
}

static std::string bytes_to_base64(const crowdy::Bytes& b) {
  if (b.size() == 0) return std::string("\"\"");
  auto v = crowdy::core::base64Encode(b);
  // return JSON string literal
  std::ostringstream o;
  o << '"' << v << '"';
  return o.str();
}

static std::string spatial_notification_to_json(const crowdy::replication::SpatialNotification& n) {
  std::ostringstream o;
  o << "{\"type\":\"" << static_cast<int>(n.type) << "\",";
  o << "\"appId\":" << n.appId << ",";
  o << "\"chunk\":" << chunk_to_json(n.chunk) << ",";
  // uuid as hex
  o << "\"uuid\":\"" << actor_uuid_to_hex(n.uuidArray()) << "\",";
  // payload base64
  o << "\"payload\":";
  if (n.payload.size() > 0) {
	std::string encoded = crowdy::core::base64Encode(n.payload);
	o << '"' << encoded << '"';
  } else {
	o << "\"\"";
  }
  o << ",\"epochMillis\":" << n.epochMillis << ",\"sequence\":" << (int)n.sequence << "}";
  return o.str();
}

class CrowdyNative : public RefCounted {
	GDCLASS(CrowdyNative, RefCounted)

private:
	std::unique_ptr<crowdy::CrowdyClient> identityClient;
	std::unique_ptr<crowdy::CrowdyClient> gameClient;

	// Replication connection managed by this native wrapper (one at a time)
	std::shared_ptr<crowdy::replication::Connection> replConn;
	// Optional WorldSession for higher-level session stores
	std::unique_ptr<crowdy::session::WorldSession> worldSession;

	// Godot-side callbacks registered for replication events/status
	Callable replication_event_cb;
	Callable replication_status_cb;

	// Lifetime / generation guard to prevent callbacks after shutdown or
	// when a connection has been replaced. Increment conn_generation_ to
	// invalidate outstanding background operations. alive_ is also checked
	// by deferred invocations.
	std::atomic_uint64_t conn_generation_ {0};
	std::atomic_bool alive_ {true};

	// Track known remote actors (hex uuid -> lastSeenMs) so we can emit
	// lifecycle events (joined/left/updated) from poll() on the main thread.
	std::unordered_map<std::string, std::int64_t> remote_actor_last_seen_;

public:

	CrowdyNative() {
		crowdy::ClientConfig cfg;
		// Ensure we have a synchronous transport to back the threaded async
		// adapter. CrowdyClient would create a curl transport internally when
		// cfg.transport is null, but we need the same sync transport instance
		// to drive the background threads here.
		cfg.transport = crowdy::graphql::makeCurlTransport();
		cfg.asyncTransport = std::make_shared<ThreadedAsyncTransport>(cfg.transport);
		cfg.httpUrl = "https://api.dev.crowdedkingdoms.com/graphql";
		identityClient = std::make_unique<crowdy::CrowdyClient>(cfg);
	}

	void poll() {
		// Single owner: native.poll() drives all SDK pumping. Do not call
		// Connection::poll() or WorldSession::tick() from elsewhere.
		if (identityClient)
		{
			identityClient->poll();
		}

		if (gameClient)
		{
			gameClient->poll();
		}
		
		if (replConn) {
			// Connection::poll() dispatches into handlers synchronously on the
			// caller thread. WorldSession::tick() also calls connection.poll() as
			// part of its single-threaded drive; to avoid double-dispatch we only
			// call worldSession->tick() when a WorldSession exists, and skip the
			// separate replConn->poll() in that case.
			if (worldSession) {
				worldSession->tick();
			} else {
				replConn->poll();
			}
		}

		uint64_t gen = conn_generation_.load(std::memory_order_acquire);
		// Lifecycle detection: if we have a WorldSession, compare current
		// remote actor list to our cached map and emit joined/left/updated
		// events via the replication_event_cb. This runs on the poll caller
		// (main thread) so it's safe to access WorldSession stores.
		if (worldSession && replication_event_cb.is_valid()) {
			// Build current map
			auto list = worldSession->actors().list();
			std::unordered_set<std::string> current;
			for (const auto* a : list) {
				std::string hex = actor_uuid_to_hex(a->uuid);
				current.insert(hex);
				auto it = remote_actor_last_seen_.find(hex);
				if (it == remote_actor_last_seen_.end()) {
					// New actor: emit joined
					std::ostringstream js;
					js << "{\"lifecycle\":\"joined\",";
					js << "\"uuid\":\"" << hex << "\",";
					js << "\"chunk\":" << chunk_to_json(a->chunk) << ",";
					if (!a->samples.empty()) {
						auto& s = a->samples.front();
						auto enc = crowdy::core::base64Encode(s.state.bytes());
						js << "\"state\":\"" << enc << "\",";
					} else {
						js << "\"state\":\"\",";
					}
					js << "\"lastSeenMs\":" << a->lastSeenMs << ",";
					js << "\"lastServerEpochMs\":" << a->lastServerEpochMs << "}";
					if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
						this->call_deferred("_invoke_callable", Callable(replication_event_cb), String(js.str().c_str()));
					}
					remote_actor_last_seen_[hex] = a->lastSeenMs;
				} else {
					// Existing actor: check for update
					if (it->second != a->lastSeenMs) {
						std::ostringstream js;
						js << "{\"lifecycle\":\"updated\",";
						js << "\"uuid\":\"" << hex << "\",";
						js << "\"chunk\":" << chunk_to_json(a->chunk) << ",";
						if (!a->samples.empty()) {
							auto& s = a->samples.front();
							auto enc = crowdy::core::base64Encode(s.state.bytes());
							js << "\"state\":\"" << enc << "\",";
						} else {
							js << "\"state\":\"\",";
						}
						js << "\"lastSeenMs\":" << a->lastSeenMs << ",";
						js << "\"lastServerEpochMs\":" << a->lastServerEpochMs << "}";
						if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
							this->call_deferred("_invoke_callable", Callable(replication_event_cb), String(js.str().c_str()));
						}
						it->second = a->lastSeenMs;
					}
				}
			}
			// Detect removals
			std::vector<std::string> to_remove;
			for (auto &kv : remote_actor_last_seen_) {
				if (current.find(kv.first) == current.end()) {
					// Actor left
					std::ostringstream js;
					js << "{\"lifecycle\":\"left\",";
					js << "\"uuid\":\"" << kv.first << "\"}";
					if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
						this->call_deferred("_invoke_callable", Callable(replication_event_cb), String(js.str().c_str()));
					}
					to_remove.push_back(kv.first);
				}
			}
			for (auto &k : to_remove) remote_actor_last_seen_.erase(k);
		}
	}


	void initialize(const String& management_url) {
		crowdy::ClientConfig cfg;

		cfg.httpUrl = (std::string)management_url.utf8();
		cfg.transport = crowdy::graphql::makeCurlTransport();
		cfg.asyncTransport = std::make_shared<ThreadedAsyncTransport>(cfg.transport);

		identityClient = std::make_unique<crowdy::CrowdyClient>(cfg);
	}

	void initializeGameClient(const crowdy::domains::AppTokenResponse& token) {
		crowdy::ClientConfig cfg;

		cfg.httpUrl = (std::string)token.gameApiUrl;
		cfg.discoveryUrl = (std::string)token.discoveryUrl;
		cfg.transport = crowdy::graphql::makeCurlTransport();
		cfg.asyncTransport = std::make_shared<ThreadedAsyncTransport>(cfg.transport);

		gameClient = std::make_unique<crowdy::CrowdyClient>(cfg);
		gameClient->setToken(token.token);
	}

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("poll"), &CrowdyNative::poll);
		ClassDB::bind_method(D_METHOD("_invoke_callable", "callback", "res"), &CrowdyNative::_invoke_callable);
		ClassDB::bind_method(D_METHOD("initialize", "management_url"), &CrowdyNative::initialize);
		ClassDB::bind_method(D_METHOD("graphql_query", "endpoint", "query"), &CrowdyNative::graphql_query);
		ClassDB::bind_method(D_METHOD("login", "email", "password"), &CrowdyNative::login);
		ClassDB::bind_method(D_METHOD("login_async", "email", "password", "callback"), &CrowdyNative::login_async);
		ClassDB::bind_method(D_METHOD("register_user", "email", "password", "gamertag"), &CrowdyNative::register_user);
		ClassDB::bind_method(D_METHOD("register_user_async", "email", "password", "gamertag", "callback"), &CrowdyNative::register_user_async);
		ClassDB::bind_method(D_METHOD("logout"), &CrowdyNative::logout);
		ClassDB::bind_method(D_METHOD("logout_async", "callback"), &CrowdyNative::logout_async);
		ClassDB::bind_method(D_METHOD("request_login_link", "email", "redirect_uri"), &CrowdyNative::request_login_link);

		// Replication bindings
		ClassDB::bind_method(D_METHOD("replication_set_event_callback", "callback"), &CrowdyNative::replication_set_event_callback);
		ClassDB::bind_method(D_METHOD("replication_set_status_callback", "callback"), &CrowdyNative::replication_set_status_callback);
		ClassDB::bind_method(D_METHOD("replication_clear_event_callback"), &CrowdyNative::replication_clear_event_callback);
		ClassDB::bind_method(D_METHOD("replication_clear_status_callback"), &CrowdyNative::replication_clear_status_callback);
		ClassDB::bind_method(D_METHOD("replication_connect", "app_id"), &CrowdyNative::replication_connect);
		ClassDB::bind_method(D_METHOD("replication_connect_async", "app_id", "callback"), &CrowdyNative::replication_connect_async);
		ClassDB::bind_method(D_METHOD("replication_disconnect"), &CrowdyNative::replication_disconnect);
		ClassDB::bind_method(D_METHOD("replication_get_connection_status"), &CrowdyNative::replication_get_connection_status);
		ClassDB::bind_method(D_METHOD("replication_create_world_session", "app_id"), &CrowdyNative::replication_create_world_session);
		ClassDB::bind_method(D_METHOD("worldsession_get_self_uuid"), &CrowdyNative::worldsession_get_self_uuid);
		ClassDB::bind_method(D_METHOD("worldsession_dispose"), &CrowdyNative::worldsession_dispose);
		ClassDB::bind_method(D_METHOD("worldsession_get_actors"), &CrowdyNative::worldsession_get_actors);
		ClassDB::bind_method(D_METHOD("replication_send_actor_update", "uuid_hex", "x", "y", "z", "state_base64"), &CrowdyNative::replication_send_actor_update);
		ClassDB::bind_method(D_METHOD("replication_send_actor_update_and_wait", "uuid_hex", "x", "y", "z", "state_base64", "timeout_ms"), &CrowdyNative::replication_send_actor_update_and_wait);
		ClassDB::bind_method(D_METHOD("replication_send_actor_update_and_wait_async", "uuid_hex", "x", "y", "z", "state_base64", "timeout_ms", "callback"), &CrowdyNative::replication_send_actor_update_and_wait_async);
		ClassDB::bind_method(D_METHOD("replication_send_heartbeat", "uuid_hex", "x", "y", "z"), &CrowdyNative::replication_send_heartbeat);
		ClassDB::bind_method(D_METHOD("replication_send_channel_message", "channel_id", "uuid_hex", "payload_base64"), &CrowdyNative::replication_send_channel_message);
		ClassDB::bind_method(D_METHOD("replication_send_single_actor_message", "x", "y", "z", "target_uuid_hex", "payload_base64"), &CrowdyNative::replication_send_single_actor_message);
	}

	String login(const String& email, const String& password) {
		try {
			auto auth = identityClient->auth().login((std::string)email.utf8(), (std::string)password.utf8());

			std::ostringstream o;
			o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
			o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
			o << "\"email\":\"" << auth.email.valueOrEmpty() << "\",";
			o << "\"gamertag\":\"" << auth.gamertag.valueOrEmpty() << "\"}}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void login_async(const String& email, const String& password, const Callable& cb) {
		try {
			identityClient->auth().loginAsync((std::string)email.utf8(), (std::string)password.utf8(),
				[this, cb](graphql::GraphQLOutcome out, crowdy::domains::AuthResponse auth) mutable {
					std::ostringstream o;
					if (out.ok()) {
						o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
						o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
						o << "\"email\":\"" << auth.email.valueOrEmpty() << "\",";
						o << "\"gamertag\":\"" << auth.gamertag.valueOrEmpty() << "\"}}";
					}
					else {
						o << "{\"ok\":false}";
					}
					// Ensure callback runs on the main thread
					this->call_deferred("_invoke_callable", Callable(cb), String(o.str().c_str()));
				});
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			this->call_deferred("_invoke_callable", Callable(cb), String(err.c_str()));
		}
	}

	String register_user(const String& email, const String& password, const String& gamertag) {
		try {
			auto auth = identityClient->auth().registerUser((std::string)email.utf8(), (std::string)password.utf8(), (std::string)gamertag.utf8());

			std::ostringstream o;
			o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
			o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
			o << "\"email\":\"" << auth.email.valueOrEmpty() << "\",";
			o << "\"gamertag\":\"" << auth.gamertag.valueOrEmpty() << "\"}}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void register_user_async(const String& email, const String& password, const String& gamertag, const Callable& cb) {
		try {
			identityClient->auth().registerUserAsync((std::string)email.utf8(), (std::string)password.utf8(), (std::string)gamertag.utf8(),
				[this, cb](graphql::GraphQLOutcome out, crowdy::domains::AuthResponse auth) mutable {
					std::ostringstream o;
					if (out.ok()) {
						o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
						o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
						o << "\"email\":\"" << auth.email.valueOrEmpty() << "\",";
						o << "\"gamertag\":\"" << auth.gamertag.valueOrEmpty() << "\"}}";
					}
					else {
						o << "{\"ok\":false}";
					}
					this->call_deferred("_invoke_callable", Callable(cb), String(o.str().c_str()));
				});
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			this->call_deferred("_invoke_callable", Callable(cb), String(err.c_str()));
		}
	}

	String logout() {
		try {
			bool ok = identityClient->auth().logout();

			std::ostringstream o;
			o << "{\"ok\":true,\"result\":" << (ok ? "true" : "false") << "}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void logout_async(const Callable& cb) {
		try {
			identityClient->auth().logoutAsync([this, cb](graphql::GraphQLOutcome out, bool ok) mutable {
				std::ostringstream o;
				o << "{\"ok\":true,\"result\":" << (ok ? "true" : "false") << "}";
				this->call_deferred("_invoke_callable", Callable(cb), String(o.str().c_str()));
				});
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			this->call_deferred("_invoke_callable", Callable(cb), String(err.c_str()));
		}
	}

	void _invoke_callable(const Callable& cb, const String& res) {
		// Only invoke if still alive. This prevents deferred callbacks from
		// running after the wrapper has been disposed.
		if (!alive_.load(std::memory_order_acquire)) return;
		// Also ensure the callback is valid
		if (!cb.is_valid()) return;
		cb.call(Variant(res));
	}

	String graphql_query(const String& endpoint, const String& query) {
		// Basic passthrough (synchronous). This is small convenience helper for
		// quick tests; prefer the higher-level helpers below for auth flows.
		try {
			auto res = identityClient->graphqlClient().request((std::string)query.utf8());
			return String(res.dump().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}


	// Request a magic login link (passwordless). Returns the server response JSON.
	String request_login_link(const String& email, const String& redirect_uri) {
		try {
			auto resp = identityClient->auth().requestLoginLink((std::string)email.utf8(), (std::string)redirect_uri.utf8());
			return String(resp.dump().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	// ----- Replication API -------------------------------------------------

	// Return our own actor uuid (hex) if a WorldSession exists, else empty string
	String worldsession_get_self_uuid() {
		if (!worldSession) return String("");
		try {
			auto u = worldSession->actorUuid();
			return String(actor_uuid_to_hex(u).c_str());
		}
		catch (...) {
			return String("");
		}
	}

	// Register a Godot Callable to receive replication events (JSON string).
	void replication_set_event_callback(const Callable& cb) { replication_event_cb = cb; }

	// Register a Godot Callable to receive status changes (ConnState integer).
	void replication_set_status_callback(const Callable& cb) { replication_status_cb = cb; }

	// Patch alignment: no-op insertion to align file for future edits.

	// Synchronous connect: mint app token and connect (blocking assign/connect call). TODO FIX
	String replication_connect(const String& app_id) {
		try {
			// New connection generation
			uint64_t gen = conn_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;

			std::string app = (std::string)app_id.utf8();

			auto token = identityClient->portal().mintAppToken(app);

			crowdy::replication::Config cfg;
			cfg.appId = static_cast<std::int64_t>(std::stoll(token.appId));
			cfg.token.token = token.token;
			cfg.token.gameTokenId = static_cast<std::int64_t>(std::stoll(token.gameTokenId));


			crowdy::replication::Handlers handlers;


			handlers.actorUpdate = [this, gen](const crowdy::replication::SpatialNotification& n) {

				if (!alive_.load(std::memory_order_acquire))
					return;

				if (gen != conn_generation_.load(std::memory_order_acquire))
					return;


				if (replication_event_cb.is_valid()) {
					std::string js = spatial_notification_to_json(n);

					this->call_deferred(
						"_invoke_callable",
						Callable(replication_event_cb),
						String(js.c_str())
					);
				}
				};


			handlers.status = [this, gen](crowdy::replication::ConnState s) {

				if (!alive_.load(std::memory_order_acquire))
					return;

				if (gen != conn_generation_.load(std::memory_order_acquire))
					return;


				if (replication_status_cb.is_valid()) {

					std::ostringstream o;
					o << "{\"status\":"
						<< static_cast<int>(s)
						<< "}";


					this->call_deferred(
						"_invoke_callable",
						Callable(replication_status_cb),
						String(o.str().c_str())
					);
				}
				};


			auto conn = identityClient->replication().connect(cfg, handlers);


			if (!alive_.load(std::memory_order_acquire) ||
				gen != conn_generation_.load(std::memory_order_acquire))
			{
				conn->disconnect();
				return String("{\"ok\":false,\"error\":\"connection cancelled\"}");
			}


			replConn = conn;


			return String("{\"ok\":true}");

		}
		catch (const std::exception& e) {

			std::string err =
				std::string("{\"ok\":false,\"error\":\"")
				+ e.what()
				+ "\"}";

			return String(err.c_str());
		}
	}

	// Asynchronous connect. connect_cb will receive JSON result. Event/status callbacks
	// should be registered via replication_set_event_callback and replication_set_status_callback.
	void replication_connect_async(const String& app_id, const Callable& connect_cb) {
		try {
			std::string app = (std::string)app_id.utf8();
			// First mint app token asynchronously
			identityClient->portal().mintAppTokenAsync(app, [this, connect_cb](crowdy::graphql::GraphQLOutcome out, crowdy::domains::AppTokenResponse token) mutable {
				if (!out.ok()) {
					std::ostringstream o;
					o << "{\"ok\":false,\"error\":\""
						<< out.errorMessage
						<< "\",\"status\":"
						<< crowdy::errcName(out.status.code)
						<< "}";

					this->call_deferred(
						"_invoke_callable",
						Callable(connect_cb),
						String(o.str().c_str())
					);
					return;
				}

				// Run the blocking connect on a background thread
				// Increment generation for this new connect attempt. Any
				// background work started by prior generations will see the
				// mismatch and avoid delivering callbacks.
				uint64_t new_gen = conn_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
				std::thread([this, connect_cb, token, new_gen]() mutable {
					try {
						crowdy::replication::Config cfg;
						cfg.appId = static_cast<std::int64_t>(std::stoll(token.appId));
						cfg.token.token = token.token;
						// UtilityFunctions::print("connection token: ", String(cfg.token.token.c_str()));
						cfg.token.gameTokenId = static_cast<std::int64_t>(std::stoll(token.gameTokenId));
						// Build handlers using stored callables
						crowdy::replication::Handlers handlers;
						handlers.actorUpdate = [this](const crowdy::replication::SpatialNotification& n) {
							std::string js = spatial_notification_to_json(n);
							if (replication_event_cb.is_valid()) this->call_deferred("_invoke_callable", Callable(replication_event_cb), String(js.c_str()));
							};
						handlers.status = [this](crowdy::replication::ConnState s) {
							std::ostringstream o; o << "{\"status\":" << static_cast<int>(s) << "}";
							if (replication_status_cb.is_valid()) this->call_deferred("_invoke_callable", Callable(replication_status_cb), String(o.str().c_str()));
							};

						initializeGameClient(token);

						auto result = gameClient->replication().connectWithStatus(cfg, handlers);
						// Replace connection atomically
						replConn = result.connection;
						auto status = result.status;
						//UtilityFunctions::print("connectWithStatus STATUS: ", static_cast<int>(status.code));

						String stateStr;

						switch (replConn->state())
						{
						case crowdy::replication::ConnState::Idle:
							stateStr = "Idle";
							break;
						case crowdy::replication::ConnState::Connecting:
							stateStr = "Connecting";
							break;
						case crowdy::replication::ConnState::Connected:
							stateStr = "Connected";
							break;
						case crowdy::replication::ConnState::Reconnecting:
							stateStr = "Reconnecting";
							break;
						case crowdy::replication::ConnState::Failed:
							stateStr = "Failed";
							break;
						case crowdy::replication::ConnState::Closed:
							stateStr = "Closed";
							break;
						}

						// Notify caller on main thread
						if (alive_.load(std::memory_order_acquire) && new_gen == conn_generation_.load(std::memory_order_acquire)) {
							this->call_deferred("_invoke_callable", Callable(connect_cb), String("{\"ok\":true}"));
						}
					}
					catch (const std::exception& e) {
						std::ostringstream o; o << "{\"ok\":false,\"error\":\"" << e.what() << "\"}";
						this->call_deferred("_invoke_callable", Callable(connect_cb), String(o.str().c_str()));
					}
					}).detach();
				});
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			this->call_deferred("_invoke_callable", Callable(connect_cb), String(err.c_str()));
		}
	}

	void replication_disconnect() {
		// Mark wrapper as not alive to prevent deferred callbacks from
		// running while we tear down underlying SDK objects.
		alive_.store(false, std::memory_order_release);
		// Bump generation to invalidate any background work in-flight.
		conn_generation_.fetch_add(1, std::memory_order_acq_rel);
		if (replConn) {
			replConn->disconnect();
			replConn.reset();
		}
		if (worldSession) {
			worldSession->dispose();
			worldSession.reset();
		}
		// Clear callbacks so future connect attempts do not reuse stale callables
		replication_event_cb = Callable();
		replication_status_cb = Callable();
		// Finally mark object alive again for possible future connect attempts
		alive_.store(true, std::memory_order_release);
	}

	void replication_clear_event_callback() { replication_event_cb = Callable(); }
	void replication_clear_status_callback() { replication_status_cb = Callable(); }

	// Return connection status as JSON: {ok:true,state:"Connected"}
	String replication_get_connection_status() {
		if (!replConn) return String("{\"ok\":false,\"error\":\"no connection\"}");
		auto s = replConn->state();
		std::ostringstream o; o << "{\"ok\":true,\"state\":\"" << crowdy::replication::connStateName(s) << "\"}";
		return String(o.str().c_str());
	}

	// Create a WorldSession over the current connection. Uses default config.
	String replication_create_world_session(const String& app_id) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"no connection\"}");
		try {
			crowdy::session::WorldSessionConfig cfg;
			cfg.appId = (std::string)app_id.utf8();
			worldSession = std::make_unique<crowdy::session::WorldSession>(replConn, identityClient.get(), cfg);
			return String("{\"ok\":true}");
		}
		catch (const std::exception& e) {
			std::ostringstream o; o << "{\"ok\":false,\"error\":\"" << e.what() << "\"}";
			return String(o.str().c_str());
		}
	}

	void worldsession_dispose() {
		if (worldSession) {
			worldSession->dispose();
			worldSession.reset();
		}
	}

	// Return JSON array of remote actors from the WorldSession actors() store.
	String worldsession_get_actors() {
		if (!worldSession) return String("[]");
		auto list = worldSession->actors().list();
		std::ostringstream o;
		o << "[";
		bool first = true;
		for (const auto* a : list) {
			if (!first) o << ",";
			first = false;
			o << "{\"uuid\":\"" << actor_uuid_to_hex(a->uuid) << "\",";
			o << "\"chunk\":" << chunk_to_json(a->chunk) << ",";
			o << "\"lastSeenMs\":" << a->lastSeenMs << ",";
			o << "\"lastServerEpochMs\":" << a->lastServerEpochMs << ",";
			// state = first sample's state base64
			if (!a->samples.empty()) {
				auto& s = a->samples.front();
				auto enc = crowdy::core::base64Encode(s.state.bytes());
				o << "\"state\":\"" << enc << "\"";
			}
			else {
				o << "\"state\":\"\"";
			}
			o << "}";
		}
		o << "]";
		return String(o.str().c_str());
	}

	// Send an actor update. Returns {ok:true,sequence:n} or {ok:false,error:..}
	String replication_send_actor_update(const String& uuid_hex, int x, int y, int z, const String& state_base64) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"not connected\"}");


		String stateStr;

		switch (replConn->state())
		{
		case crowdy::replication::ConnState::Idle:
			stateStr = "Idle";
			break;
		case crowdy::replication::ConnState::Connecting:
			stateStr = "Connecting";
			break;
		case crowdy::replication::ConnState::Connected:
			stateStr = "Connected";
			break;
		case crowdy::replication::ConnState::Reconnecting:
			stateStr = "Reconnecting";
			break;
		case crowdy::replication::ConnState::Failed:
			stateStr = "Failed";
			break;
		case crowdy::replication::ConnState::Closed:
			stateStr = "Closed";
			break;
		}

		crowdy::core::ActorUuid uuid{};
		if (!actor_uuid_from_hex((std::string)uuid_hex.utf8(), uuid)) return String("{\"ok\":false,\"error\":\"invalid uuid\"}");
		auto v = crowdy::core::base64Decode((std::string)state_base64.utf8());
		if (!v.has_value()) return String("{\"ok\":false,\"error\":\"invalid base64\"}");
		std::vector<std::uint8_t>& bytes = *v;
		crowdy::replication::SpatialSend p;
		p.chunk.x = x; p.chunk.y = y; p.chunk.z = z;
		p.uuid = uuid;
		p.payload = crowdy::Bytes(bytes.data(), bytes.size());
		auto seq = replConn->sendActorUpdate(p);
		if (!seq.ok()) {
			std::ostringstream o; o << "{\"ok\":false,\"error\":\"send failed\",\"code\":\"" << (int)seq.error() << "\"}";
			return String(o.str().c_str());
		}
		std::ostringstream o; o << "{\"ok\":true,\"sequence\":" << (int)seq.value() << "}";
		return String(o.str().c_str());
	}

	// Send actor update and wait for echo or error (blocking). Returns JSON with acknowledged, error and serverEpochMs.
	String replication_send_actor_update_and_wait(const String& uuid_hex, int x, int y, int z, const String& state_base64, int timeout_ms) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"not connected\"}");
		crowdy::core::ActorUuid uuid{};
		if (!actor_uuid_from_hex((std::string)uuid_hex.utf8(), uuid)) return String("{\"ok\":false,\"error\":\"invalid uuid\"}");
		auto v = crowdy::core::base64Decode((std::string)state_base64.utf8());
		if (!v.has_value()) return String("{\"ok\":false,\"error\":\"invalid base64\"}");
		std::vector<std::uint8_t>& bytes = *v;
		crowdy::replication::SpatialSend p;
		p.chunk.x = x; p.chunk.y = y; p.chunk.z = z;
		p.uuid = uuid;
		p.payload = crowdy::Bytes(bytes.data(), bytes.size());
		auto out = replConn->sendActorUpdateAndWait(p, timeout_ms);
		std::ostringstream o;
		o << "{\"ok\":true,\"acknowledged\":" << (out.acknowledged ? "true" : "false") << ",";
		if (out.error.has_value()) o << "\"error\":\"" << static_cast<int>(out.error.value()) << "\",";
		o << "\"serverEpochMs\":" << out.serverEpochMs << "}";
		return String(o.str().c_str());
	}

	// Async version of send_actor_update_and_wait: runs on background thread and calls cb with JSON result
	void replication_send_actor_update_and_wait_async(const String& uuid_hex, int x, int y, int z, const String& state_base64, int timeout_ms, const Callable& cb) {
		if (!replConn) {
			this->call_deferred("_invoke_callable", Callable(cb), String("{\"ok\":false,\"error\":\"not connected\"}"));
			return;
		}
		// Capture copy of connection pointer and current generation to guard
		// against delivering callbacks after a disconnect/reconnect.
		auto conn = replConn;
		uint64_t gen = conn_generation_.load(std::memory_order_acquire);
		std::string suuid = (std::string)uuid_hex.utf8();
		std::string sstate = (std::string)state_base64.utf8();
		std::thread([this, conn, suuid, x, y, z, sstate, timeout_ms, cb, gen]() mutable {
			crowdy::core::ActorUuid uuid{};
			if (!actor_uuid_from_hex(suuid, uuid)) {
				if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
					this->call_deferred("_invoke_callable", Callable(cb), String("{\"ok\":false,\"error\":\"invalid uuid\"}"));
				}
				return;
			}
			auto v = crowdy::core::base64Decode(sstate);
			if (!v.has_value()) {
				if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
					this->call_deferred("_invoke_callable", Callable(cb), String("{\"ok\":false,\"error\":\"invalid base64\"}"));
				}
				return;
			}
			std::vector<std::uint8_t>& bytes = *v;
			crowdy::replication::SpatialSend p;
			p.chunk.x = x; p.chunk.y = y; p.chunk.z = z;
			p.uuid = uuid;
			p.payload = crowdy::Bytes(bytes.data(), bytes.size());
			auto out = conn->sendActorUpdateAndWait(p, timeout_ms);
			std::ostringstream o;
			o << "{\"ok\":true,\"acknowledged\":" << (out.acknowledged ? "true" : "false") << ",";
			if (out.error.has_value()) o << "\"error\":\"" << static_cast<int>(out.error.value()) << "\",";
			o << "\"serverEpochMs\":" << out.serverEpochMs << "}";
			if (alive_.load(std::memory_order_acquire) && gen == conn_generation_.load(std::memory_order_acquire)) {
				this->call_deferred("_invoke_callable", Callable(cb), String(o.str().c_str()));
			}
			}).detach();
	}

	// Heartbeat
	String replication_send_heartbeat(const String& uuid_hex, int x, int y, int z) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"not connected\"}");
		crowdy::core::ActorUuid uuid{};
		if (!actor_uuid_from_hex((std::string)uuid_hex.utf8(), uuid)) return String("{\"ok\":false,\"error\":\"invalid uuid\"}");
		crowdy::wire::ChunkCoord chunk; chunk.x = x; chunk.y = y; chunk.z = z;
		auto seq = replConn->sendHeartbeat(chunk, uuid);
		if (!seq.ok()) return String("{\"ok\":false,\"error\":\"send failed\"}");
		std::ostringstream o; o << "{\"ok\":true,\"sequence\":" << (int)seq.value() << "}";
		return String(o.str().c_str());
	}

	// Channel message
	String replication_send_channel_message(int64_t channel_id, const String& uuid_hex, const String& payload_base64) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"not connected\"}");
		crowdy::core::ActorUuid uuid{};
		if (!actor_uuid_from_hex((std::string)uuid_hex.utf8(), uuid)) return String("{\"ok\":false,\"error\":\"invalid uuid\"}");
		auto v = crowdy::core::base64Decode((std::string)payload_base64.utf8());
		if (!v.has_value()) return String("{\"ok\":false,\"error\":\"invalid base64\"}");
		std::vector<std::uint8_t>& bytes = *v;
		auto res = replConn->sendChannelMessage(channel_id, uuid, crowdy::Bytes(bytes.data(), bytes.size()));
		if (!res.ok()) return String("{\"ok\":false,\"error\":\"send failed\"}");
		std::ostringstream o; o << "{\"ok\":true,\"sequence\":" << (int)res.value() << "}";
		return String(o.str().c_str());
	}

	// Single actor direct message
	String replication_send_single_actor_message(int x, int y, int z, const String& target_uuid_hex, const String& payload_base64) {
		if (!replConn) return String("{\"ok\":false,\"error\":\"not connected\"}");
		crowdy::core::ActorUuid target{};
		if (!actor_uuid_from_hex((std::string)target_uuid_hex.utf8(), target)) return String("{\"ok\":false,\"error\":\"invalid uuid\"}");
		auto v = crowdy::core::base64Decode((std::string)payload_base64.utf8());
		if (!v.has_value()) return String("{\"ok\":false,\"error\":\"invalid base64\"}");
		std::vector<std::uint8_t>& bytes = *v;
		crowdy::wire::ChunkCoord chunk; chunk.x = x; chunk.y = y; chunk.z = z;
		auto res = replConn->sendSingleActorMessage(chunk, target, crowdy::Bytes(bytes.data(), bytes.size()));
		if (!res.ok()) return String("{\"ok\":false,\"error\":\"send failed\"}");
		std::ostringstream o; o << "{\"ok\":true,\"sequence\":" << (int)res.value() << "}";
		return String(o.str().c_str());
	}

	// Parse hex string (2 chars per byte) into ActorUuid. Returns true on success.
	static bool actor_uuid_from_hex(const std::string& hex, crowdy::core::ActorUuid& out) {
		if (hex.size() != crowdy::wire::kUuidSize * 2) return false;
		for (size_t i = 0; i < crowdy::wire::kUuidSize; ++i) {
			std::string byte_hex = hex.substr(i * 2, 2);
			try {
				unsigned int val = std::stoul(byte_hex, nullptr, 16);
				out.data()[i] = static_cast<unsigned char>(val);
			}
			catch (...) {
				return false;
			}
		}
		return true;
	}


};

extern "C" void initialize_crowdy_gdextension(ModuleInitializationLevel p_level) {
  if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) return;
  ClassDB::register_class<CrowdyNative>();
}

extern "C" void uninitialize_crowdy_gdextension(ModuleInitializationLevel p_level) {
  (void)p_level;
}

extern "C" {
GDExtensionBool GDE_EXPORT crowdy_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_crowdy_gdextension);
	init_obj.register_terminator(uninitialize_crowdy_gdextension);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
