#pragma once

#include <chrono>
#include <future>
#include <optional>
#include <thread>
#include <utility>

namespace crowdy_godot {

// Runs one blocking call on a thread of its own, so Godot's main thread never waits on it, and
// hands the result back to the main thread through take().
//
// The call must not touch Godot objects or the members of its owner: it captures copies of what
// it needs. Destroying a BackgroundCall waits for the call to return, because a detached thread
// could still be running code from the extension's library after Godot unloads it.
template <typename T>
class BackgroundCall {
public:
	BackgroundCall() = default;
	BackgroundCall(const BackgroundCall &) = delete;
	BackgroundCall &operator=(const BackgroundCall &) = delete;

	~BackgroundCall() {
		if (thread.joinable()) {
			thread.join();
		}
	}

	// True from start() until take() has returned the result.
	bool is_running() const { return future.valid(); }

	// Returns false, and starts nothing, while a previous call is still running.
	template <typename F>
	bool start(F &&p_call) {
		if (is_running()) {
			return false;
		}
		std::packaged_task<T()> task(std::forward<F>(p_call));
		future = task.get_future();
		thread = std::thread(std::move(task));
		return true;
	}

	// The result once the call has returned; nothing while it is still running.
	std::optional<T> take() {
		if (!future.valid() || future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
			return std::nullopt;
		}
		thread.join();
		return future.get();
	}

private:
	std::future<T> future;
	std::thread thread;
};

} // namespace crowdy_godot
