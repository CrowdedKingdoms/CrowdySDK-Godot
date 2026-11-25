#include "crowdy_request.h"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;

namespace crowdy_godot {

void CrowdyRequest::complete(const Ref<CrowdyResult> &p_result) {
	ERR_FAIL_COND_MSG(completed, "A CrowdyRequest completes only once.");
	completed = true;
	emit_signal("completed", p_result);
}

void CrowdyRequest::_bind_methods() {
	ADD_SIGNAL(MethodInfo("completed", PropertyInfo(Variant::OBJECT, "result", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_DEFAULT, "CrowdyResult")));
}

std::pair<uint64_t, Ref<CrowdyRequest>> PendingRequests::create() {
	Ref<CrowdyRequest> request;
	request.instantiate();
	const uint64_t id = next_id++;
	pending.emplace(id, request);
	return { id, request };
}

void PendingRequests::resolve(uint64_t p_id, const Ref<CrowdyResult> &p_result) {
	resolved.emplace_back(p_id, p_result);
}

void PendingRequests::flush() {
	// A script reacting to one completion may start requests or clear this queue, so take one
	// entry at a time instead of iterating.
	while (!resolved.empty()) {
		auto [id, result] = std::move(resolved.front());
		resolved.pop_front();

		const auto found = pending.find(id);
		if (found == pending.end()) {
			continue;
		}
		const Ref<CrowdyRequest> request = found->second;
		pending.erase(found);
		request->complete(result);
	}
}

void PendingRequests::clear() {
	resolved.clear();
	pending.clear();
}

} // namespace crowdy_godot
