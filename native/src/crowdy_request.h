#pragma once

#include "crowdy_result.h"

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include <cstdint>
#include <deque>
#include <unordered_map>
#include <utility>

namespace crowdy_godot {

// Handle to one asynchronous operation. It emits `completed(result)` exactly once, always from
// a later poll() of the object that created it, so `await request.completed` placed right after
// the call can never miss it.
class CrowdyRequest : public godot::RefCounted {
	GDCLASS(CrowdyRequest, godot::RefCounted)

public:
	void complete(const godot::Ref<CrowdyResult> &p_result);

protected:
	static void _bind_methods();

private:
	bool completed = false;
};

// The requests an object has handed out and not yet completed.
//
// SDK callbacks capture a request's id, never the request: a callback can be copied to and
// destroyed on an SDK worker thread, and a Godot object must only be released on the main
// thread. resolve() records the result and flush() emits it after the SDK has returned, so the
// script that reacts to `completed` never runs inside SDK code.
class PendingRequests {
public:
	std::pair<uint64_t, godot::Ref<CrowdyRequest>> create();
	void resolve(uint64_t p_id, const godot::Ref<CrowdyResult> &p_result);
	void flush();
	// Drops every pending request without completing it.
	void clear();

private:
	uint64_t next_id = 1;
	std::unordered_map<uint64_t, godot::Ref<CrowdyRequest>> pending;
	std::deque<std::pair<uint64_t, godot::Ref<CrowdyResult>>> resolved;
};

} // namespace crowdy_godot
