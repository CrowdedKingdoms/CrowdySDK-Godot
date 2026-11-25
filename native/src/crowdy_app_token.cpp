#include "crowdy_app_token.h"

#include "conversions.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace crowdy_godot {

Ref<CrowdyAppToken> CrowdyAppToken::wrap(crowdy::domains::AppTokenResponse p_response, std::string p_issuing_origin) {
	Ref<CrowdyAppToken> token;
	token.instantiate();
	token->response = std::move(p_response);
	token->issuing_origin = std::move(p_issuing_origin);
	return token;
}

String CrowdyAppToken::get_app_id() const {
	return to_godot(response.appId);
}

String CrowdyAppToken::get_expires_at() const {
	return to_godot(response.expiresAt);
}

bool CrowdyAppToken::is_paused() const {
	return crowdy::domains::isAppPaused(response.runtimeGate);
}

void CrowdyAppToken::_bind_methods() {
	ClassDB::bind_method(D_METHOD("get_app_id"), &CrowdyAppToken::get_app_id);
	ClassDB::bind_method(D_METHOD("get_expires_at"), &CrowdyAppToken::get_expires_at);
	ClassDB::bind_method(D_METHOD("is_paused"), &CrowdyAppToken::is_paused);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "app_id"), "", "get_app_id");
	// ISO-8601; empty when the token does not expire.
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "expires_at"), "", "get_expires_at");
	// A paused app still mints tokens, but the realtime servers refuse its sends (APP_PAUSED).
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "paused"), "", "is_paused");
}

} // namespace crowdy_godot
