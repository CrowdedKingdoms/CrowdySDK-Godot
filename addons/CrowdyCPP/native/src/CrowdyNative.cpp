#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>
#include <sstream>
#include <memory>

#include <crowdy/crowdy.hpp>
#include <gdextension_interface.h>

using namespace godot;
namespace graphql = crowdy::graphql;

class CrowdyNative : public RefCounted {
  GDCLASS(CrowdyNative, RefCounted)

private:
	std::unique_ptr<crowdy::CrowdyClient> client;

public:

	CrowdyNative() {
		crowdy::ClientConfig cfg;
		client = std::make_unique<crowdy::CrowdyClient>(cfg);
	}

	void poll() { client->poll(); }

	void set_management_url(const String& management_url) {
		crowdy::ClientConfig cfg;
		cfg.managementUrl = (std::string)management_url.utf8();
		client = std::make_unique<crowdy::CrowdyClient>(cfg);
	}

	static void _bind_methods() {
		ClassDB::bind_method(D_METHOD("poll"), &CrowdyNative::poll);
		ClassDB::bind_method(D_METHOD("set_management_url", "management_url"), &CrowdyNative::set_management_url);
		ClassDB::bind_method(D_METHOD("graphql_query", "endpoint", "query"), &CrowdyNative::graphql_query);
		ClassDB::bind_method(D_METHOD("dev_login", "email"), &CrowdyNative::dev_login);
		ClassDB::bind_method(D_METHOD("dev_login_async", "email"), &CrowdyNative::dev_login_async);
		ClassDB::bind_method(D_METHOD("login", "email", "password"), &CrowdyNative::login);
		ClassDB::bind_method(D_METHOD("login_async", "email", "password", "callback"), &CrowdyNative::login_async);
		ClassDB::bind_method(D_METHOD("register_user", "email", "password", "gamertag"), &CrowdyNative::register_user);
		ClassDB::bind_method(D_METHOD("register_user_async", "email", "password", "gamertag", "callback"), &CrowdyNative::register_user_async);
		ClassDB::bind_method(D_METHOD("logout"), &CrowdyNative::logout);
		ClassDB::bind_method(D_METHOD("logout_async", "callback"), &CrowdyNative::logout_async);
		ClassDB::bind_method(D_METHOD("request_login_link", "email", "redirect_uri"), &CrowdyNative::request_login_link);
		ClassDB::bind_method(D_METHOD("_invoke_callable", "callback", "res"), &CrowdyNative::_invoke_callable);
	}

	// DEV-only convenience: perform devLogin(email) against the management URL
	// and return a small JSON result with token and user info.
	String dev_login(const String& email) {
		try {
			auto auth = client->auth().devLogin((std::string)email.utf8());

			std::ostringstream o;
			o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
			o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
			o << "\"email\":\"" << auth.email << "\",";
			o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void dev_login_async(const String& email, const Callable& cb) {
		try {
			client->auth().devLoginAsync((std::string)email.utf8(),
				[this, cb](graphql::GraphQLOutcome out, crowdy::domains::AuthResponse auth) mutable {
					std::ostringstream o;
					if (out.ok()) {
						o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
						o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
						o << "\"email\":\"" << auth.email << "\",";
						o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
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

	String login(const String& email, const String& password) {
		try {
			auto auth = client->auth().login((std::string)email.utf8(), (std::string)password.utf8());

			std::ostringstream o;
			o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
			o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
			o << "\"email\":\"" << auth.email << "\",";
			o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void login_async(const String& email, const String& password, const Callable& cb) {
		try {
			client->auth().loginAsync((std::string)email.utf8(), (std::string)password.utf8(),
				[this, cb](graphql::GraphQLOutcome out, crowdy::domains::AuthResponse auth) mutable {
					std::ostringstream o;
					if (out.ok()) {
						o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
						o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
						o << "\"email\":\"" << auth.email << "\",";
						o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
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
			auto auth = client->auth().registerUser((std::string)email.utf8(), (std::string)password.utf8(), (std::string)gamertag.utf8());

			std::ostringstream o;
			o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
			o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
			o << "\"email\":\"" << auth.email << "\",";
			o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
			return String(o.str().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
	}

	void register_user_async(const String& email, const String& password, const String& gamertag, const Callable& cb) {
		try {
			client->auth().registerUserAsync((std::string)email.utf8(), (std::string)password.utf8(), (std::string)gamertag.utf8(),
				[this, cb](graphql::GraphQLOutcome out, crowdy::domains::AuthResponse auth) mutable {
					std::ostringstream o;
					if (out.ok()) {
						o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
						o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
						o << "\"email\":\"" << auth.email << "\",";
						o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
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
			bool ok = client->auth().logout();

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
			client->auth().logoutAsync([this, cb](graphql::GraphQLOutcome out, bool ok) mutable {
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
		// Directly call the provided callable on the main thread
		cb.call(Variant(res));
	}

	String graphql_query(const String& endpoint, const String& query) {
		// Basic passthrough (synchronous). This is small convenience helper for
		// quick tests; prefer the higher-level helpers below for auth flows.
		try {
			auto res = client->managementClient().request((std::string)query.utf8());
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
			auto resp = client->auth().requestLoginLink((std::string)email.utf8(), (std::string)redirect_uri.utf8());
			return String(resp.dump().c_str());
		}
		catch (const std::exception& e) {
			std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
			return String(err.c_str());
		}
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
