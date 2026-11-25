#include "crowdy_result.h"

#include "conversions.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace crowdy_godot {

Ref<CrowdyResult> CrowdyResult::success(const Variant &p_data) {
	Ref<CrowdyResult> result;
	result.instantiate();
	result->ok = true;
	result->data = p_data;
	return result;
}

Ref<CrowdyResult> CrowdyResult::failure(const String &p_error_code, const String &p_error_message) {
	Ref<CrowdyResult> result;
	result.instantiate();
	result->error_code = p_error_code;
	result->error_message = p_error_message;
	return result;
}

Ref<CrowdyResult> CrowdyResult::failure_from(const crowdy::graphql::GraphQLOutcome &p_outcome) {
	using Kind = crowdy::graphql::GraphQLErrorKind;

	// The platform's own code (extensions.code) is what a game branches on.
	if (!p_outcome.errors.empty()) {
		const crowdy::graphql::GraphQLErrorDetail &first = p_outcome.errors.front();
		const String code = first.code.empty() ? String("GRAPHQL_ERROR") : to_godot(first.code);
		return failure(code, to_godot(first.message));
	}

	const String message = to_godot(p_outcome.errorMessage);
	switch (p_outcome.kind) {
		case Kind::Http:
			return failure("HTTP_ERROR", message.is_empty() ? "HTTP status " + String::num_int64(p_outcome.httpStatus) : message);
		case Kind::Timeout:
			return failure("TIMEOUT", message);
		case Kind::Network:
			return failure("NETWORK_ERROR", message);
		case Kind::Cancelled:
			return failure("CANCELLED", message);
		case Kind::Protocol:
			return failure("PROTOCOL_ERROR", message);
		case Kind::GraphQL:
			return failure("GRAPHQL_ERROR", message);
		case Kind::None:
			break;
	}
	return failure_from(p_outcome.status.code, message);
}

Ref<CrowdyResult> CrowdyResult::failure_from(crowdy::Errc p_code, const String &p_error_message) {
	const String message = p_error_message.is_empty() ? String(crowdy::errcName(p_code)) : p_error_message;
	return failure(error_code_of(p_code), message);
}

String CrowdyResult::_to_string() const {
	if (ok) {
		return "<CrowdyResult OK>";
	}
	return "<CrowdyResult " + error_code + ": " + error_message + ">";
}

void CrowdyResult::_bind_methods() {
	ClassDB::bind_static_method("CrowdyResult", D_METHOD("success", "data"), &CrowdyResult::success, DEFVAL(Variant()));
	ClassDB::bind_static_method("CrowdyResult", D_METHOD("failure", "error_code", "error_message"), &CrowdyResult::failure);

	ClassDB::bind_method(D_METHOD("is_ok"), &CrowdyResult::is_ok);
	ClassDB::bind_method(D_METHOD("get_error_code"), &CrowdyResult::get_error_code);
	ClassDB::bind_method(D_METHOD("get_error_message"), &CrowdyResult::get_error_message);
	ClassDB::bind_method(D_METHOD("get_data"), &CrowdyResult::get_data);

	// Read-only: a result describes something that already happened.
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "ok"), "", "is_ok");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_code"), "", "get_error_code");
	ADD_PROPERTY(PropertyInfo(Variant::STRING, "error_message"), "", "get_error_message");
	ADD_PROPERTY(PropertyInfo(Variant::NIL, "data", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT), "", "get_data");
}

} // namespace crowdy_godot
