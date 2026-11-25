#pragma once

#include <crowdy/client.hpp>

namespace crowdy_godot {

// The CrowdyClient configuration every client of this extension starts from: the libcurl
// transport, a worker thread for the *Async calls (without one they would run inline and block
// Godot's main thread), and a request timeout. Origins are left for the caller to set.
crowdy::ClientConfig make_client_config();

} // namespace crowdy_godot
