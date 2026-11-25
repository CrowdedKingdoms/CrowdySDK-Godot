#pragma once

#include "crowdy_request.h"

#include <crowdy/client.hpp>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

#include <memory>

namespace crowdy_godot {

// The player's platform account: signing in, the gates a new account passes before it may play
// (email confirmation, legal documents, age), and minting the app tokens that start game
// sessions. Wraps the identity CrowdyClient on the shared API origin; game sessions run on
// their own per-app client (see CrowdyGameSession).
//
// Every operation is asynchronous and returns a CrowdyRequest. Results are delivered from
// poll(), which the owner calls once per frame on the main thread.
class CrowdyIdentity : public godot::RefCounted {
	GDCLASS(CrowdyIdentity, godot::RefCounted)

public:
	// An empty api_url selects the SDK's default origin. With persist_session the session token
	// is kept in user:// so the player stays signed in across launches.
	static godot::Ref<CrowdyIdentity> create(const godot::String &p_api_url, bool p_persist_session);

	void poll();
	bool is_signed_in() const;

	godot::Ref<CrowdyRequest> sign_in(const godot::String &p_email, const godot::String &p_password);
	godot::Ref<CrowdyRequest> register_user(const godot::String &p_email, const godot::String &p_password, const godot::String &p_gamertag);
	godot::Ref<CrowdyRequest> sign_out();
	godot::Ref<CrowdyRequest> fetch_profile();

	godot::Ref<CrowdyRequest> resend_confirmation_email(const godot::String &p_email);
	godot::Ref<CrowdyRequest> request_password_reset(const godot::String &p_email);
	godot::Ref<CrowdyRequest> fetch_legal_documents(const godot::String &p_app_id);
	godot::Ref<CrowdyRequest> accept_legal_documents(const godot::String &p_app_id);
	godot::Ref<CrowdyRequest> record_player_age(const godot::String &p_birth_date, const godot::String &p_country, bool p_parental_confirmation);

	godot::Ref<CrowdyRequest> mint_app_token(const godot::String &p_app_id);

protected:
	static void _bind_methods();

private:
	std::unique_ptr<crowdy::CrowdyClient> client;
	PendingRequests requests;
};

} // namespace crowdy_godot
