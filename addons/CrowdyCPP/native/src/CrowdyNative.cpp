#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>

#include <string>
#include <sstream>

#include <crowdy/crowdy.hpp>
#include <gdextension_interface.h>

using namespace godot;

class CrowdyNative : public RefCounted {
  GDCLASS(CrowdyNative, RefCounted)

public:
  static void _bind_methods() {
	ClassDB::bind_method(D_METHOD("ping"), &CrowdyNative::ping);
	ClassDB::bind_method(D_METHOD("graphql_query", "endpoint", "query"), &CrowdyNative::graphql_query);
	ClassDB::bind_method(D_METHOD("dev_login", "management_url", "email"), &CrowdyNative::dev_login);
	ClassDB::bind_method(D_METHOD("request_login_link", "management_url", "email", "redirect_uri"), &CrowdyNative::request_login_link);
  }

  String ping() {
	return String("pong");
  }

	String graphql_query(const String &endpoint, const String &query) {
	// Basic passthrough (synchronous). This is small convenience helper for
	// quick tests; prefer the higher-level helpers below for auth flows.
	try {
	  crowdy::ClientConfig cfg;
	  cfg.managementUrl = (std::string)endpoint.utf8();
		crowdy::CrowdyClient client(cfg);
	  auto res = client.managementClient().request((std::string)query.utf8());
	  return String(res.dump().c_str());
	} catch (const std::exception &e) {
	  std::string err = std::string("{\"error\":\"") + e.what() + "\"}";
	  return String(err.c_str());
	}
  }

  // DEV-only convenience: perform devLogin(email) against the management URL
  // and return a small JSON result with token and user info.
  String dev_login(const String &management_url, const String &email) {
	try {
	  crowdy::ClientConfig cfg;
	  cfg.managementUrl = (std::string)management_url.utf8();
	  crowdy::CrowdyClient client(cfg);
	  auto auth = client.auth().devLogin((std::string)email.utf8());

	  std::ostringstream o;
	  o << "{\"ok\":true,\"token\":\"" << auth.token << "\",";
	  o << "\"user\":{\"userId\":\"" << auth.userId << "\",";
	  o << "\"email\":\"" << auth.email << "\",";
	  o << "\"gamertag\":\"" << auth.gamertag << "\"}}";
	  return String(o.str().c_str());
	} catch (const std::exception &e) {
	  std::string err = std::string("{\"ok\":false,\"error\":\"") + e.what() + "\"}";
	  return String(err.c_str());
	}
  }

  // Request a magic login link (passwordless). Returns the server response JSON.
  String request_login_link(const String &management_url, const String &email, const String &redirect_uri) {
	try {
	  crowdy::ClientConfig cfg;
	  cfg.managementUrl = (std::string)management_url.utf8();
	  crowdy::CrowdyClient client(cfg);
		auto resp = client.auth().requestLoginLink((std::string)email.utf8(), (std::string)redirect_uri.utf8());
	  return String(resp.dump().c_str());
	} catch (const std::exception &e) {
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
