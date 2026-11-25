#pragma once

#include <crowdy/domains/types.hpp>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>

namespace crowdy_godot {

// An app-scoped gameplay token, as CrowdyIdentity.mint_app_token() returns it. It is opaque on
// purpose: the token is also the HMAC key of the realtime connection, so scripts pass it to
// CrowdyGameSession.start() and never need to read or log it.
class CrowdyAppToken : public godot::RefCounted {
	GDCLASS(CrowdyAppToken, godot::RefCounted)

public:
	// p_issuing_origin is the API origin the token came from, where a game session goes when the
	// token names no datacenter of its own.
	static godot::Ref<CrowdyAppToken> wrap(crowdy::domains::AppTokenResponse p_response, std::string p_issuing_origin);

	const crowdy::domains::AppTokenResponse &get_response() const { return response; }
	const std::string &get_issuing_origin() const { return issuing_origin; }

	godot::String get_app_id() const;
	godot::String get_expires_at() const;
	bool is_paused() const;

protected:
	static void _bind_methods();

private:
	crowdy::domains::AppTokenResponse response;
	std::string issuing_origin;
};

} // namespace crowdy_godot
