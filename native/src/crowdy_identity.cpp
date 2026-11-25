#include "crowdy_identity.h"

#include "client_config.h"
#include "conversions.h"
#include "crowdy_app_token.h"

#include <crowdy/default_origin.hpp>
#include <crowdy/graphql/auth_state.hpp>

#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

#include <vector>

using namespace godot;

namespace crowdy_godot {

namespace {

using crowdy::graphql::GraphQLOutcome;

Dictionary user_of(const crowdy::domains::AuthResponse &p_auth) {
	Dictionary user;
	user["user_id"] = to_godot(p_auth.userId);
	user["email"] = to_godot(p_auth.email.valueOrEmpty());
	user["gamertag"] = to_godot(p_auth.gamertag.valueOrEmpty());
	return user;
}

Dictionary profile_of(const crowdy::graphql::Json &p_me) {
	Dictionary profile;
	profile["user_id"] = to_godot(p_me["userId"].asBigIntString());
	profile["email"] = to_godot(p_me["email"].asStringView());
	profile["gamertag"] = to_godot(p_me["gamertag"].asStringView());
	profile["disambiguation"] = to_godot(p_me["disambiguation"].asStringView());
	profile["is_confirmed"] = p_me["isConfirmed"].asBool();
	// "unknown", "under_13", "13_15", "16_17" or "18_plus": for game logic, never shown to players.
	profile["age_bracket"] = to_godot(p_me["ageBracket"].asStringView());
	profile["country"] = to_godot(p_me["country"].asStringView());
	return profile;
}

Array documents_of(const std::vector<crowdy::domains::LegalDocument> &p_documents) {
	Array documents;
	for (const crowdy::domains::LegalDocument &document : p_documents) {
		Dictionary entry;
		entry["document_id"] = to_godot(document.documentId);
		entry["version"] = to_godot(document.version);
		entry["title"] = to_godot(document.title);
		entry["url"] = to_godot(document.url);
		// Empty for the platform's documents, the app's id for an app's own.
		entry["app_id"] = to_godot(document.appId.valueOrEmpty());
		documents.push_back(entry);
	}
	return documents;
}

} // namespace

Ref<CrowdyIdentity> CrowdyIdentity::create(const String &p_api_url, bool p_persist_session) {
	crowdy::ClientConfig config = make_client_config();
	config.httpUrl = to_std(p_api_url.strip_edges());

	if (p_persist_session) {
		// One session file per origin, as the SDK names it, so games sharing an origin share
		// the sign-in.
		const std::string origin = config.httpUrl.empty() ? std::string(crowdy::kDefaultHttpOrigin) : config.httpUrl;
		const String directory = ProjectSettings::get_singleton()->globalize_path("user://");
		config.tokenStore = std::make_shared<crowdy::graphql::FileTokenStore>(
				crowdy::graphql::FileTokenStore::sessionPath(to_std(directory), origin));
	}

	Ref<CrowdyIdentity> identity;
	identity.instantiate();
	identity->client = std::make_unique<crowdy::CrowdyClient>(std::move(config));
	if (p_persist_session) {
		identity->client->restoreSession();
	}
	return identity;
}

void CrowdyIdentity::poll() {
	// A script reacting to a result may drop its last reference to this object.
	const Ref<CrowdyIdentity> keep_alive(this);
	client->poll();
	requests.flush();
}

bool CrowdyIdentity::is_signed_in() const {
	return client->authState().hasToken();
}

Ref<CrowdyRequest> CrowdyIdentity::sign_in(const String &p_email, const String &p_password) {
	auto [id, request] = requests.create();
	client->auth().loginAsync(to_std(p_email), to_std(p_password),
			[this, id](GraphQLOutcome p_outcome, crowdy::domains::AuthResponse p_auth) {
				requests.resolve(id, p_outcome.ok() ? CrowdyResult::success(user_of(p_auth)) : CrowdyResult::failure_from(p_outcome));
			});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::register_user(const String &p_email, const String &p_password, const String &p_gamertag) {
	auto [id, request] = requests.create();
	client->auth().registerUserAsync(to_std(p_email), to_std(p_password), to_std(p_gamertag),
			[this, id](GraphQLOutcome p_outcome, crowdy::domains::AuthResponse p_auth) {
				requests.resolve(id, p_outcome.ok() ? CrowdyResult::success(user_of(p_auth)) : CrowdyResult::failure_from(p_outcome));
			});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::sign_out() {
	auto [id, request] = requests.create();
	// The request carries the session token from the moment it is built, so the local session
	// ends now whatever the server answers.
	client->auth().logoutAsync([this, id](GraphQLOutcome p_outcome, bool) {
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success() : CrowdyResult::failure_from(p_outcome));
	});
	client->authState().clearToken();
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::fetch_profile() {
	auto [id, request] = requests.create();
	client->users().meAsync([this, id](GraphQLOutcome p_outcome) {
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success(profile_of(p_outcome.data)) : CrowdyResult::failure_from(p_outcome));
	});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::resend_confirmation_email(const String &p_email) {
	auto [id, request] = requests.create();
	client->auth().resendConfirmationEmailAsync(to_std(p_email), [this, id](GraphQLOutcome p_outcome, bool) {
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success() : CrowdyResult::failure_from(p_outcome));
	});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::request_password_reset(const String &p_email) {
	auto [id, request] = requests.create();
	client->auth().requestPasswordResetAsync(to_std(p_email), [this, id](GraphQLOutcome p_outcome, bool) {
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success() : CrowdyResult::failure_from(p_outcome));
	});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::fetch_legal_documents(const String &p_app_id) {
	auto [id, request] = requests.create();
	const std::string app_id = to_std(p_app_id.strip_edges());
	client->auth().playerLegalDocumentsAsync(
			[this, id, app_id](GraphQLOutcome p_outcome, std::vector<crowdy::domains::LegalDocument> p_documents) {
				if (!p_outcome.ok()) {
					requests.resolve(id, CrowdyResult::failure_from(p_outcome));
					return;
				}
				if (app_id.empty()) {
					requests.resolve(id, CrowdyResult::success(documents_of(p_documents)));
					return;
				}
				// The app's own documents are accepted together with the platform's.
				client->auth().appRequiredDocumentsAsync(app_id,
						[this, id, documents = std::move(p_documents)](GraphQLOutcome p_app_outcome, std::vector<crowdy::domains::LegalDocument> p_app_documents) mutable {
							if (!p_app_outcome.ok()) {
								requests.resolve(id, CrowdyResult::failure_from(p_app_outcome));
								return;
							}
							documents.insert(documents.end(), p_app_documents.begin(), p_app_documents.end());
							requests.resolve(id, CrowdyResult::success(documents_of(documents)));
						});
			});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::accept_legal_documents(const String &p_app_id) {
	auto [id, request] = requests.create();
	crowdy::domains::PlayerConsents consents;
	consents.acceptLegal = true;
	consents.appId = to_std(p_app_id.strip_edges());
	consents.method = crowdy::gen::LegalAcceptanceMethod::IN_GAME;
	client->auth().recordPlayerConsentsAsync(consents, [this, id](GraphQLOutcome p_outcome, bool) {
		requests.resolve(id, p_outcome.ok() ? CrowdyResult::success() : CrowdyResult::failure_from(p_outcome));
	});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::record_player_age(const String &p_birth_date, const String &p_country, bool p_parental_confirmation) {
	auto [id, request] = requests.create();
	crowdy::domains::PlayerAgeInput input;
	input.birthDate = to_std(p_birth_date.strip_edges());
	input.country = to_std(p_country.strip_edges().to_upper());
	input.parentalConfirmation = p_parental_confirmation;
	client->auth().recordPlayerAgeAsync(input, [this, id](GraphQLOutcome p_outcome, crowdy::domains::PlayerAge p_age) {
		if (!p_outcome.ok()) {
			requests.resolve(id, CrowdyResult::failure_from(p_outcome));
			return;
		}
		Dictionary age;
		age["age_bracket"] = to_godot(p_age.ageBracket.valueOrEmpty());
		age["country"] = to_godot(p_age.country.valueOrEmpty());
		requests.resolve(id, CrowdyResult::success(age));
	});
	return request;
}

Ref<CrowdyRequest> CrowdyIdentity::mint_app_token(const String &p_app_id) {
	auto [id, request] = requests.create();
	client->portal().mintAppTokenAsync(to_std(p_app_id.strip_edges()),
			[this, id](GraphQLOutcome p_outcome, crowdy::domains::AppTokenResponse p_token) {
				if (!p_outcome.ok()) {
					requests.resolve(id, CrowdyResult::failure_from(p_outcome));
					return;
				}
				requests.resolve(id, CrowdyResult::success(CrowdyAppToken::wrap(std::move(p_token), client->config().httpUrl)));
			});
	return request;
}

void CrowdyIdentity::_bind_methods() {
	ClassDB::bind_static_method("CrowdyIdentity", D_METHOD("create", "api_url", "persist_session"), &CrowdyIdentity::create, DEFVAL(true));

	ClassDB::bind_method(D_METHOD("poll"), &CrowdyIdentity::poll);
	ClassDB::bind_method(D_METHOD("is_signed_in"), &CrowdyIdentity::is_signed_in);

	ClassDB::bind_method(D_METHOD("sign_in", "email", "password"), &CrowdyIdentity::sign_in);
	ClassDB::bind_method(D_METHOD("register_user", "email", "password", "gamertag"), &CrowdyIdentity::register_user, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("sign_out"), &CrowdyIdentity::sign_out);
	ClassDB::bind_method(D_METHOD("fetch_profile"), &CrowdyIdentity::fetch_profile);

	ClassDB::bind_method(D_METHOD("resend_confirmation_email", "email"), &CrowdyIdentity::resend_confirmation_email);
	ClassDB::bind_method(D_METHOD("request_password_reset", "email"), &CrowdyIdentity::request_password_reset);
	ClassDB::bind_method(D_METHOD("fetch_legal_documents", "app_id"), &CrowdyIdentity::fetch_legal_documents, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("accept_legal_documents", "app_id"), &CrowdyIdentity::accept_legal_documents, DEFVAL(String()));
	ClassDB::bind_method(D_METHOD("record_player_age", "birth_date", "country", "parental_confirmation"), &CrowdyIdentity::record_player_age, DEFVAL(false));

	ClassDB::bind_method(D_METHOD("mint_app_token", "app_id"), &CrowdyIdentity::mint_app_token);
}

} // namespace crowdy_godot
