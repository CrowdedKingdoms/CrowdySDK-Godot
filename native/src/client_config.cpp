#include "client_config.h"

#include <crowdy/graphql/http.hpp>

namespace crowdy_godot {

namespace {

// Also bounds how long destroying a client waits: its async worker finishes the request in
// flight before it stops. The SDK's default of 60 s would stall a game's shutdown that long.
constexpr long kRequestTimeoutMs = 15000;

} // namespace

crowdy::ClientConfig make_client_config() {
	crowdy::ClientConfig config;
	config.transport = crowdy::graphql::makeCurlTransport();
	config.asyncTransport = crowdy::graphql::makeThreadedAsyncTransport(config.transport);
	config.timeoutMs = kRequestTimeoutMs;
	return config;
}

} // namespace crowdy_godot
