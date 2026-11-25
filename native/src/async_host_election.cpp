#include "async_host_election.h"

namespace crowdy_godot {

AsyncHostElection::AsyncHostElection(crowdy::CrowdyClient &p_client) :
		client(p_client) {
}

crowdy::session::IHostElection::Beat AsyncHostElection::heartbeat(const std::string &p_app_id) {
	if (!state->in_flight) {
		state->in_flight = true;
		// Both answers are delivered from client.poll(), so the client is alive when they run.
		crowdy::CrowdyClient *host_client = &client;
		client.host().heartbeatAsync(p_app_id, [state = state, host_client, p_app_id](crowdy::graphql::GraphQLOutcome p_beat) {
			if (!p_beat.ok()) {
				state->in_flight = false;
				return;
			}
			std::string host_user_id = p_beat.data["hostUserId"].asBigIntString();
			host_client->host().amIHostAsync(p_app_id, [state, host_user_id = std::move(host_user_id)](crowdy::graphql::GraphQLOutcome p_check, bool p_am_host) {
				state->in_flight = false;
				if (p_check.ok()) {
					state->last = Beat{ true, host_user_id, p_am_host };
				}
			});
		});
	}
	// Until the first answer arrives this is a failed beat, which leaves the host unknown.
	return state->last;
}

} // namespace crowdy_godot
