#pragma once

#include <crowdy/client.hpp>
#include <crowdy/session/world_session.hpp>

#include <memory>
#include <string>

namespace crowdy_godot {

// Host election that never blocks the game thread.
//
// WorldSession asks its IHostElection for a heartbeat from tick(), and the SDK's own adapter
// answers with two blocking GraphQL calls, which would stall Godot's main thread every interval.
// This one starts the calls asynchronously and answers with the last result it has, so the host
// it reports trails the server by at most one interval.
class AsyncHostElection final : public crowdy::session::IHostElection {
public:
	// The client must outlive this object; its poll() delivers the answers.
	explicit AsyncHostElection(crowdy::CrowdyClient &p_client);

	Beat heartbeat(const std::string &p_app_id) override;

private:
	struct State {
		bool in_flight = false;
		Beat last;
	};

	crowdy::CrowdyClient &client;
	// Shared with the callbacks, which may outlive this object.
	std::shared_ptr<State> state = std::make_shared<State>();
};

} // namespace crowdy_godot
