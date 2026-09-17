#include "CrowdyNative.hpp"
#include "CrowdySerialization.hpp"

#include <crowdy/crowdy.hpp>
#include <crowdy/replication/connection.hpp>
#include <crowdy/session/world_session.hpp>

#include <sstream>
#include <thread>
#include <vector>

using namespace godot;

// -----------------------------------------------------------------------------
// WorldSession
// -----------------------------------------------------------------------------

String CrowdyNative::worldsession_get_self_uuid() 
{
	if (!worldSession) {
		return String("");
	}

	try {
		auto uuid = worldSession->actorUuid();
		return String(actor_uuid_to_hex(uuid).c_str());
	}
	catch (...) {
		return String("");
	}
}

// -----------------------------------------------------------------------------
// Replication callbacks
// -----------------------------------------------------------------------------

void CrowdyNative::replication_set_event_callback(
	const Callable& cb
) {
	replication_event_cb = cb;
}

void CrowdyNative::replication_set_status_callback(
	const Callable& cb
) {
	replication_status_cb = cb;
}

void CrowdyNative::replication_clear_event_callback() {
	replication_event_cb = Callable();
}

void CrowdyNative::replication_clear_status_callback() {
	replication_status_cb = Callable();
}

// -----------------------------------------------------------------------------
// Synchronous replication connect
// -----------------------------------------------------------------------------

String CrowdyNative::replication_connect(
	const String& app_id
) {
	try {
		// Start a new connection generation.
		uint64_t gen = conn_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
		std::string app = (std::string)app_id.utf8().ptr();

		auto token = identityClient->portal().mintAppToken(app);

		crowdy::replication::Config cfg;

		cfg.appId = static_cast<std::int64_t>(std::stoll(token.appId));
		cfg.token.token = token.token;
		cfg.token.gameTokenId = static_cast<std::int64_t>(std::stoll(token.gameTokenId));

		crowdy::replication::Handlers handlers;

		// -----------------------------------------------------------------
		// Actor update callback
		// -----------------------------------------------------------------

		handlers.actorUpdate =
			[this, gen](
				const crowdy::replication::SpatialNotification& notification
				) {
					if (!alive_.load(
						std::memory_order_acquire
					)) {
						return;
					}

					if (gen != conn_generation_.load(
						std::memory_order_acquire
					)) {
						return;
					}

					if (!replication_event_cb.is_valid()) {
						return;
					}

					std::string json = spatial_notification_to_json(notification);

					this->call_deferred(
						"_invoke_callable",
						Callable(replication_event_cb),
						String(json.c_str())
					);
			};

		// -----------------------------------------------------------------
		// Connection status callback
		// -----------------------------------------------------------------

		handlers.status =
			[this, gen](
				crowdy::replication::ConnState state
				) {
					if (!alive_.load(
						std::memory_order_acquire
					)) {
						return;
					}

					if (gen != conn_generation_.load(
						std::memory_order_acquire
					)) {
						return;
					}

					if (!replication_status_cb.is_valid()) {
						return;
					}

					std::ostringstream out;

					out
						<< "{\"status\":"
						<< static_cast<int>(state)
						<< "}";

					this->call_deferred(
						"_invoke_callable",
						Callable(replication_status_cb),
						String(out.str().c_str())
					);
			};

		// -----------------------------------------------------------------
		// Connect
		// -----------------------------------------------------------------

		auto conn =
			identityClient->replication().connect(
				cfg,
				handlers
			);

		// The connection may have completed after a newer connection
		// generation was started.
		if (
			!alive_.load(std::memory_order_acquire) ||
			gen != conn_generation_.load(std::memory_order_acquire)
			) {
			conn->disconnect();

			return String(
				"{\"ok\":false,\"error\":\"connection cancelled\"}"
			);
		}

		replConn = conn;

		return String("{\"ok\":true}");
	}
	catch (const std::exception& e) {
		std::string error =
			std::string(
				"{\"ok\":false,\"error\":\""
			)
			+ e.what()
			+ "\"}";

		return String(error.c_str());
	}
}

// -----------------------------------------------------------------------------
// Asynchronous replication connect
// -----------------------------------------------------------------------------

void CrowdyNative::replication_connect_async(
	const String& app_id,
	const Callable& connect_cb
) {
	try {
		std::string app =
			(std::string)app_id.utf8().ptr();

		// Mint the application token asynchronously first.
		identityClient->portal().mintAppTokenAsync(
			app,

			[this, connect_cb](
				crowdy::graphql::GraphQLOutcome outcome,
				crowdy::domains::AppTokenResponse token
				) mutable {
					// ---------------------------------------------------------
					// App token request failed
					// ---------------------------------------------------------

					if (!outcome.ok()) {
						std::ostringstream out;

						out
							<< "{\"ok\":false,\"error\":\""
							<< outcome.errorMessage
							<< "\",\"status\":\""
							<< crowdy::errcName(outcome.status.code)
							<< "\"}";

						this->call_deferred(
							"_invoke_callable",
							Callable(connect_cb),
							String(out.str().c_str())
						);

						return;
					}

					// ---------------------------------------------------------
					// New connection generation
					// ---------------------------------------------------------

					uint64_t new_gen =
						conn_generation_.fetch_add(
							1,
							std::memory_order_acq_rel
						) + 1;

					// ---------------------------------------------------------
					// Perform blocking connection on worker thread
					// ---------------------------------------------------------

					std::thread(
						[
							this,
							connect_cb,
							token,
							new_gen
						]() mutable {
							try {
								// -------------------------------------------------
								// Check whether this attempt is still valid
								// -------------------------------------------------

								if (
									!alive_.load(
										std::memory_order_acquire
									) ||
									new_gen != conn_generation_.load(
										std::memory_order_acquire
									)
									) {
									return;
								}

								crowdy::replication::Config cfg;

								cfg.appId =
									static_cast<std::int64_t>(
										std::stoll(token.appId)
										);

								cfg.token.token =
									token.token;

								cfg.token.gameTokenId =
									static_cast<std::int64_t>(
										std::stoll(token.gameTokenId)
										);

								crowdy::replication::Handlers handlers;

								// -------------------------------------------------
								// Actor update handler
								// -------------------------------------------------

								handlers.actorUpdate =
									[this, new_gen](
										const crowdy::replication::SpatialNotification& notification
										) {
											if (!alive_.load(
												std::memory_order_acquire
											)) {
												return;
											}

											if (new_gen != conn_generation_.load(
												std::memory_order_acquire
											)) {
												return;
											}

											if (!replication_event_cb.is_valid()) {
												return;
											}

											std::string json =
												spatial_notification_to_json(
													notification
												);

											this->call_deferred(
												"_invoke_callable",
												Callable(replication_event_cb),
												String(json.c_str())
											);
									};

								// -------------------------------------------------
								// Status handler
								// -------------------------------------------------

								handlers.status =
									[this, new_gen](
										crowdy::replication::ConnState state
										) {
											if (!alive_.load(
												std::memory_order_acquire
											)) {
												return;
											}

											if (new_gen != conn_generation_.load(
												std::memory_order_acquire
											)) {
												return;
											}

											if (!replication_status_cb.is_valid()) {
												return;
											}

											std::ostringstream out;

											out
												<< "{\"status\":"
												<< static_cast<int>(state)
												<< "}";

											this->call_deferred(
												"_invoke_callable",
												Callable(replication_status_cb),
												String(out.str().c_str())
											);
									};

								// -------------------------------------------------
								// Initialize game client
								// -------------------------------------------------

								initializeGameClient(token);

								// -------------------------------------------------
								// Connect
								// -------------------------------------------------

								auto result =
									gameClient->replication()
									.connectWithStatus(
										cfg,
										handlers
									);

								// -------------------------------------------------
								// Check generation before replacing connection
								// -------------------------------------------------

								if (
									!alive_.load(
										std::memory_order_acquire
									) ||
									new_gen != conn_generation_.load(
										std::memory_order_acquire
									)
									) {
									if (result.connection) {
										result.connection->disconnect();
									}

									return;
								}

								replConn =
									result.connection;

								// -------------------------------------------------
								// Notify caller on Godot main thread
								// -------------------------------------------------

								if (
									alive_.load(
										std::memory_order_acquire
									) &&
									new_gen == conn_generation_.load(
										std::memory_order_acquire
									)
									) {
									this->call_deferred(
										"_invoke_callable",
										Callable(connect_cb),
										String("{\"ok\":true}")
									);
								}
							}
							catch (const std::exception& e) {
								if (
									!alive_.load(
										std::memory_order_acquire
									) ||
									new_gen != conn_generation_.load(
										std::memory_order_acquire
									)
									) {
									return;
								}

								std::ostringstream out;

								out
									<< "{\"ok\":false,\"error\":\""
									<< e.what()
									<< "\"}";

								this->call_deferred(
									"_invoke_callable",
									Callable(connect_cb),
									String(out.str().c_str())
								);
							}
						}
					).detach();
			}
		);
	}
	catch (const std::exception& e) {
		std::string error =
			std::string(
				"{\"ok\":false,\"error\":\""
			)
			+ e.what()
			+ "\"}";

		call_deferred(
			"_invoke_callable",
			Callable(connect_cb),
			String(error.c_str())
		);
	}
}

// -----------------------------------------------------------------------------
// Disconnect
// -----------------------------------------------------------------------------

void CrowdyNative::replication_disconnect() {
	// Invalidate callbacks from currently running work.
	alive_.store(
		false,
		std::memory_order_release
	);

	conn_generation_.fetch_add(
		1,
		std::memory_order_acq_rel
	);

	// Disconnect replication connection.
	if (replConn) {
		replConn->disconnect();
		replConn.reset();
	}

	// Dispose WorldSession before clearing the connection state.
	if (worldSession) {
		worldSession->dispose();
		worldSession.reset();
	}

	// Remove callbacks so a future connection does not accidentally
	// reuse callbacks from the previous connection.
	replication_event_cb = Callable();
	replication_status_cb = Callable();

	// Clear lifecycle tracking.
	remote_actor_last_seen_.clear();

	// Allow this CrowdyNative instance to be used again.
	alive_.store(
		true,
		std::memory_order_release
	);
}

// -----------------------------------------------------------------------------
// Connection status
// -----------------------------------------------------------------------------

String CrowdyNative::replication_get_connection_status() {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"no connection\"}"
		);
	}

	auto state =
		replConn->state();

	std::ostringstream out;

	out
		<< "{\"ok\":true,\"state\":\""
		<< crowdy::replication::connStateName(state)
		<< "\"}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// WorldSession creation
// -----------------------------------------------------------------------------

String CrowdyNative::replication_create_world_session(
	const String& app_id
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"no connection\"}"
		);
	}

	try {
		crowdy::session::WorldSessionConfig cfg;

		cfg.appId =
			(std::string)app_id.utf8().ptr();

		worldSession =
			std::make_unique<crowdy::session::WorldSession>(
				replConn,
				identityClient.get(),
				cfg
			);

		// Reset lifecycle tracking because this is a new session.
		remote_actor_last_seen_.clear();

		return String("{\"ok\":true}");
	}
	catch (const std::exception& e) {
		std::ostringstream out;

		out
			<< "{\"ok\":false,\"error\":\""
			<< e.what()
			<< "\"}";

		return String(out.str().c_str());
	}
}

// -----------------------------------------------------------------------------
// WorldSession dispose
// -----------------------------------------------------------------------------

void CrowdyNative::worldsession_dispose() {
	if (worldSession) {
		worldSession->dispose();
		worldSession.reset();
	}

	remote_actor_last_seen_.clear();
}

// -----------------------------------------------------------------------------
// WorldSession actors
// -----------------------------------------------------------------------------

String CrowdyNative::worldsession_get_actors() {
	if (!worldSession) {
		return String("[]");
	}

	auto list =
		worldSession->actors().list();

	std::ostringstream out;

	out << "[";

	bool first = true;

	for (const auto* actor : list) {
		if (!first) {
			out << ",";
		}

		first = false;

		out
			<< "{\"uuid\":\""
			<< actor_uuid_to_hex(actor->uuid)
			<< "\",";

		out
			<< "\"chunk\":"
			<< chunk_to_json(actor->chunk)
			<< ",";

		out
			<< "\"lastSeenMs\":"
			<< actor->lastSeenMs
			<< ",";

		out
			<< "\"lastServerEpochMs\":"
			<< actor->lastServerEpochMs
			<< ",";

		// First sample contains the most recent actor state.
		if (!actor->samples.empty()) {
			auto& sample =
				actor->samples.front();

			auto encoded =
				crowdy::core::base64Encode(
					sample.state.bytes()
				);

			out
				<< "\"state\":\""
				<< encoded
				<< "\"";
		}
		else {
			out << "\"state\":\"\"";
		}

		out << "}";
	}

	out << "]";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// Actor update
// -----------------------------------------------------------------------------

String CrowdyNative::replication_send_actor_update(
	const String& uuid_hex,
	int x,
	int y,
	int z,
	const String& state_base64
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"not connected\"}"
		);
	}

	crowdy::core::ActorUuid uuid{};

	if (!actor_uuid_from_hex(
		(std::string)uuid_hex.utf8().ptr(),
		uuid
	)) {
		return String(
			"{\"ok\":false,\"error\":\"invalid uuid\"}"
		);
	}

	auto decoded =
		crowdy::core::base64Decode(
			(std::string)state_base64.utf8().ptr()
		);

	if (!decoded.has_value()) {
		return String(
			"{\"ok\":false,\"error\":\"invalid base64\"}"
		);
	}

	std::vector<std::uint8_t>& bytes =
		*decoded;

	crowdy::replication::SpatialSend send;

	send.chunk.x = x;
	send.chunk.y = y;
	send.chunk.z = z;

	send.uuid = uuid;

	send.payload =
		crowdy::Bytes(
			bytes.data(),
			bytes.size()
		);

	auto sequence =
		replConn->sendActorUpdate(send);

	if (!sequence.ok()) {
		std::ostringstream out;

		out
			<< "{\"ok\":false,"
			<< "\"error\":\"send failed\","
			<< "\"code\":\""
			<< static_cast<int>(sequence.error())
			<< "\"}";

		return String(out.str().c_str());
	}

	std::ostringstream out;

	out
		<< "{\"ok\":true,\"sequence\":"
		<< static_cast<int>(sequence.value())
		<< "}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// Actor update and wait
// -----------------------------------------------------------------------------

String CrowdyNative::replication_send_actor_update_and_wait(
	const String& uuid_hex,
	int x,
	int y,
	int z,
	const String& state_base64,
	int timeout_ms
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"not connected\"}"
		);
	}

	crowdy::core::ActorUuid uuid{};

	if (!actor_uuid_from_hex(
		(std::string)uuid_hex.utf8().ptr(),
		uuid
	)) {
		return String(
			"{\"ok\":false,\"error\":\"invalid uuid\"}"
		);
	}

	auto decoded =
		crowdy::core::base64Decode(
			(std::string)state_base64.utf8().ptr()
		);

	if (!decoded.has_value()) {
		return String(
			"{\"ok\":false,\"error\":\"invalid base64\"}"
		);
	}

	std::vector<std::uint8_t>& bytes =
		*decoded;

	crowdy::replication::SpatialSend send;

	send.chunk.x = x;
	send.chunk.y = y;
	send.chunk.z = z;

	send.uuid = uuid;

	send.payload =
		crowdy::Bytes(
			bytes.data(),
			bytes.size()
		);

	auto result =
		replConn->sendActorUpdateAndWait(
			send,
			timeout_ms
		);

	std::ostringstream out;

	out
		<< "{\"ok\":true,"
		<< "\"acknowledged\":"
		<< (
			result.acknowledged
			? "true"
			: "false"
			)
		<< ",";

	if (result.error.has_value()) {
		out
			<< "\"error\":\""
			<< static_cast<int>(
				result.error.value()
				)
			<< "\",";
	}

	out
		<< "\"serverEpochMs\":"
		<< result.serverEpochMs
		<< "}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// Actor update and wait - async
// -----------------------------------------------------------------------------

void CrowdyNative::replication_send_actor_update_and_wait_async(
	const String& uuid_hex,
	int x,
	int y,
	int z,
	const String& state_base64,
	int timeout_ms,
	const Callable& cb
) {
	if (!replConn) {
		call_deferred(
			"_invoke_callable",
			Callable(cb),
			String(
				"{\"ok\":false,\"error\":\"not connected\"}"
			)
		);

		return;
	}

	// Keep the connection alive independently of replConn being replaced.
	auto conn = replConn;

	uint64_t gen =
		conn_generation_.load(
			std::memory_order_acquire
		);

	std::string uuid_string =
		(std::string)uuid_hex.utf8().ptr();

	std::string state_string =
		(std::string)state_base64.utf8().ptr();

	std::thread(
		[
			this,
			conn,
			uuid_string,
			x,
			y,
			z,
			state_string,
			timeout_ms,
			cb,
			gen
		]() mutable {
		// -------------------------------------------------------------
		// UUID
		// -------------------------------------------------------------

		crowdy::core::ActorUuid uuid{};

		if (!actor_uuid_from_hex(
			uuid_string,
			uuid
		)) {
			if (
				alive_.load(
					std::memory_order_acquire
				) &&
				gen == conn_generation_.load(
					std::memory_order_acquire
				)
				) {
				this->call_deferred(
					"_invoke_callable",
					Callable(cb),
					String(
						"{\"ok\":false,\"error\":\"invalid uuid\"}"
					)
				);
			}

			return;
		}

		// -------------------------------------------------------------
		// Base64 state
		// -------------------------------------------------------------

		auto decoded =
			crowdy::core::base64Decode(
				state_string
			);

		if (!decoded.has_value()) {
			if (
				alive_.load(
					std::memory_order_acquire
				) &&
				gen == conn_generation_.load(
					std::memory_order_acquire
				)
				) {
				this->call_deferred(
					"_invoke_callable",
					Callable(cb),
					String(
						"{\"ok\":false,\"error\":\"invalid base64\"}"
					)
				);
			}

			return;
		}

		std::vector<std::uint8_t>& bytes =
			*decoded;

		// -------------------------------------------------------------
		// Build packet
		// -------------------------------------------------------------

		crowdy::replication::SpatialSend send;

		send.chunk.x = x;
		send.chunk.y = y;
		send.chunk.z = z;

		send.uuid = uuid;

		send.payload =
			crowdy::Bytes(
				bytes.data(),
				bytes.size()
			);

		// -------------------------------------------------------------
		// Blocking SDK operation on worker thread
		// -------------------------------------------------------------

		auto result =
			conn->sendActorUpdateAndWait(
				send,
				timeout_ms
			);

		std::ostringstream out;

		out
			<< "{\"ok\":true,"
			<< "\"acknowledged\":"
			<< (
				result.acknowledged
				? "true"
				: "false"
				)
			<< ",";

		if (result.error.has_value()) {
			out
				<< "\"error\":\""
				<< static_cast<int>(
					result.error.value()
					)
				<< "\",";
		}

		out
			<< "\"serverEpochMs\":"
			<< result.serverEpochMs
			<< "}";

		// -------------------------------------------------------------
		// Return result to Godot main thread
		// -------------------------------------------------------------

		if (
			alive_.load(
				std::memory_order_acquire
			) &&
			gen == conn_generation_.load(
				std::memory_order_acquire
			)
			) {
			this->call_deferred(
				"_invoke_callable",
				Callable(cb),
				String(out.str().c_str())
			);
		}
	}
	).detach();
}

// -----------------------------------------------------------------------------
// Heartbeat
// -----------------------------------------------------------------------------

String CrowdyNative::replication_send_heartbeat(
	const String& uuid_hex,
	int x,
	int y,
	int z
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"not connected\"}"
		);
	}

	crowdy::core::ActorUuid uuid{};

	if (!actor_uuid_from_hex(
		(std::string)uuid_hex.utf8().ptr(),
		uuid
	)) {
		return String(
			"{\"ok\":false,\"error\":\"invalid uuid\"}"
		);
	}

	crowdy::wire::ChunkCoord chunk;

	chunk.x = x;
	chunk.y = y;
	chunk.z = z;

	auto sequence =
		replConn->sendHeartbeat(
			chunk,
			uuid
		);

	if (!sequence.ok()) {
		return String(
			"{\"ok\":false,\"error\":\"send failed\"}"
		);
	}

	std::ostringstream out;

	out
		<< "{\"ok\":true,\"sequence\":"
		<< static_cast<int>(sequence.value())
		<< "}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// Channel message
// -----------------------------------------------------------------------------

String CrowdyNative::replication_send_channel_message(
	int64_t channel_id,
	const String& uuid_hex,
	const String& payload_base64
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"not connected\"}"
		);
	}

	crowdy::core::ActorUuid uuid{};

	if (!actor_uuid_from_hex(
		(std::string)uuid_hex.utf8().ptr(),
		uuid
	)) {
		return String(
			"{\"ok\":false,\"error\":\"invalid uuid\"}"
		);
	}

	auto decoded =
		crowdy::core::base64Decode(
			(std::string)payload_base64.utf8().ptr()
		);

	if (!decoded.has_value()) {
		return String(
			"{\"ok\":false,\"error\":\"invalid base64\"}"
		);
	}

	std::vector<std::uint8_t>& bytes =
		*decoded;

	auto result =
		replConn->sendChannelMessage(
			channel_id,
			uuid,
			crowdy::Bytes(
				bytes.data(),
				bytes.size()
			)
		);

	if (!result.ok()) {
		return String(
			"{\"ok\":false,\"error\":\"send failed\"}"
		);
	}

	std::ostringstream out;

	out
		<< "{\"ok\":true,\"sequence\":"
		<< static_cast<int>(result.value())
		<< "}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// Single actor direct message
// -----------------------------------------------------------------------------

String CrowdyNative::replication_send_single_actor_message(
	int x,
	int y,
	int z,
	const String& target_uuid_hex,
	const String& payload_base64
) {
	if (!replConn) {
		return String(
			"{\"ok\":false,\"error\":\"not connected\"}"
		);
	}

	crowdy::core::ActorUuid target{};

	if (!actor_uuid_from_hex(
		(std::string)target_uuid_hex.utf8().ptr(),
		target
	)) {
		return String(
			"{\"ok\":false,\"error\":\"invalid uuid\"}"
		);
	}

	auto decoded =
		crowdy::core::base64Decode(
			(std::string)payload_base64.utf8().ptr()
		);

	if (!decoded.has_value()) {
		return String(
			"{\"ok\":false,\"error\":\"invalid base64\"}"
		);
	}

	std::vector<std::uint8_t>& bytes =
		*decoded;

	crowdy::wire::ChunkCoord chunk;

	chunk.x = x;
	chunk.y = y;
	chunk.z = z;

	auto result =
		replConn->sendSingleActorMessage(
			chunk,
			target,
			crowdy::Bytes(
				bytes.data(),
				bytes.size()
			)
		);

	if (!result.ok()) {
		return String(
			"{\"ok\":false,\"error\":\"send failed\"}"
		);
	}

	std::ostringstream out;

	out
		<< "{\"ok\":true,\"sequence\":"
		<< static_cast<int>(result.value())
		<< "}";

	return String(out.str().c_str());
}

// -----------------------------------------------------------------------------
// UUID conversion
// -----------------------------------------------------------------------------

bool CrowdyNative::actor_uuid_from_hex(
	const std::string& hex,
	crowdy::core::ActorUuid& out
) {
	if (
		hex.size() !=
		crowdy::wire::kUuidSize * 2
		) {
		return false;
	}

	for (
		size_t i = 0;
		i < crowdy::wire::kUuidSize;
		++i
		) {
		std::string byte_hex =
			hex.substr(i * 2, 2);

		try {
			unsigned int value =
				std::stoul(
					byte_hex,
					nullptr,
					16
				);

			out.data()[i] =
				static_cast<unsigned char>(value);
		}
		catch (...) {
			return false;
		}
	}

	return true;
}
