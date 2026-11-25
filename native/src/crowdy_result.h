#pragma once

#include <crowdy/core/result.hpp>
#include <crowdy/graphql/graphql_client.hpp>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

namespace crowdy_godot {

// The outcome of one asynchronous SDK operation: either `data` or a machine-readable
// `error_code` (a platform code such as EMAIL_CONFIRMATION_REQUIRED, or a client-side one such
// as TIMEOUT) with a human-readable `error_message`. Immutable once created.
class CrowdyResult : public godot::RefCounted {
	GDCLASS(CrowdyResult, godot::RefCounted)

public:
	static godot::Ref<CrowdyResult> success(const godot::Variant &p_data = godot::Variant());
	static godot::Ref<CrowdyResult> failure(const godot::String &p_error_code, const godot::String &p_error_message);
	static godot::Ref<CrowdyResult> failure_from(const crowdy::graphql::GraphQLOutcome &p_outcome);
	static godot::Ref<CrowdyResult> failure_from(crowdy::Errc p_code, const godot::String &p_error_message = godot::String());

	bool is_ok() const { return ok; }
	godot::String get_error_code() const { return error_code; }
	godot::String get_error_message() const { return error_message; }
	godot::Variant get_data() const { return data; }

	godot::String _to_string() const;

protected:
	static void _bind_methods();

private:
	bool ok = false;
	godot::String error_code;
	godot::String error_message;
	godot::Variant data;
};

} // namespace crowdy_godot
