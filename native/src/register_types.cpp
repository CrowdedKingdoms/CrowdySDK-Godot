// GDExtension entry point: registers the classes scripts use.

#include "crowdy_app_token.h"
#include "crowdy_game_session.h"
#include "crowdy_identity.h"
#include "crowdy_request.h"
#include "crowdy_result.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

using namespace godot;

namespace {

void initialize_crowdy_sdk(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	// The SDK creates these; scripts only receive them (CrowdyResult also has static factories).
	GDREGISTER_ABSTRACT_CLASS(crowdy_godot::CrowdyResult);
	GDREGISTER_ABSTRACT_CLASS(crowdy_godot::CrowdyRequest);
	GDREGISTER_ABSTRACT_CLASS(crowdy_godot::CrowdyAppToken);
	// Created through CrowdyIdentity.create(), so an identity always has a client.
	GDREGISTER_ABSTRACT_CLASS(crowdy_godot::CrowdyIdentity);
	GDREGISTER_CLASS(crowdy_godot::CrowdyGameSession);
}

void uninitialize_crowdy_sdk(ModuleInitializationLevel) {
}

} // namespace

extern "C" {

GDExtensionBool GDE_EXPORT crowdy_sdk_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_object(p_get_proc_address, p_library, r_initialization);
	init_object.register_initializer(initialize_crowdy_sdk);
	init_object.register_terminator(uninitialize_crowdy_sdk);
	init_object.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
	return init_object.init();
}

} // extern "C"
