// The re-discovery contract, which is mostly about what it must NOT do.
//
// Re-discovery runs while a client is already failing, so every failure mode
// here is a second failure landing on top of a first. It must not throw, must
// not move the client twice for one outage, and must not report success when it
// changed nothing — a caller that believes it recovered stops retrying.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "crowdy/graphql/rediscover.hpp"
#include "test_util.hpp"

using crowdy::graphql::RediscoveredEndpoint;
using crowdy::graphql::RediscoverCoordinator;

namespace {

void testNoCallbackMeansNoAnswer() {
  RediscoverCoordinator coordinator;
  CHECK(!coordinator.hasCallback());
  CHECK(coordinator.attempt("42").empty());
}

void testTheAnswerIsPassedThrough() {
  RediscoverCoordinator coordinator;
  std::string sawAppId;
  coordinator.setCallback([&](const std::string& appId) {
    sawAppId = appId;
    RediscoveredEndpoint endpoint;
    endpoint.httpUrl = "https://ck-or.prod.cp.cks-env.com";
    endpoint.wsUrl = "wss://ck-or.prod.cp.cks-env.com";
    return endpoint;
  });
  const auto answer = coordinator.attempt("42");
  CHECK_EQ(sawAppId, "42");
  CHECK_EQ(answer.httpUrl, "https://ck-or.prod.cp.cks-env.com");
  CHECK(!answer.empty());
}

void testAnEmptyAnswerIsNotAMove() {
  RediscoverCoordinator coordinator;
  coordinator.setCallback([](const std::string&) {
    return RediscoveredEndpoint{};
  });
  CHECK(coordinator.attempt("42").empty());
}

#ifndef CROWDY_NO_EXCEPTIONS
void testAThrowingCallbackIsNotFatal() {
  // The caller is inside a reconnect path with nowhere to put a second failure.
  RediscoverCoordinator coordinator;
  coordinator.setCallback([](const std::string&) -> RediscoveredEndpoint {
    throw std::runtime_error("discovery origin is down too");
  });
  CHECK(coordinator.attempt("42").empty());

  // And the coordinator is still usable afterwards: a throw must not leave the
  // in-flight flag stuck, which would disable re-discovery for the process.
  coordinator.setCallback([](const std::string&) {
    RediscoveredEndpoint endpoint;
    endpoint.httpUrl = "https://ck-va.prod.cp.cks-env.com";
    return endpoint;
  });
  CHECK(!coordinator.attempt("42").empty());
}
#endif

void testConcurrentAttemptsShareOne() {
  // The subscription socket, the UDP watchdog and a failed request all notice a
  // dead endpoint at once. Without coalescing each would ask and each would
  // apply its own answer, moving a struggling client repeatedly.
  RediscoverCoordinator coordinator;
  std::atomic<int> calls{0};
  std::atomic<bool> release{false};
  coordinator.setCallback([&](const std::string&) {
    ++calls;
    // A BOUNDED wait, so that a coordinator without coalescing fails the call
    // count below instead of deadlocking. A test that hangs reports as a
    // timeout, which says far less than the assertion it replaced.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!release.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    RediscoveredEndpoint endpoint;
    endpoint.httpUrl = "https://ck-or.prod.cp.cks-env.com";
    return endpoint;
  });

  std::thread first([&] { (void)coordinator.attempt("42"); });
  // Wait until the first attempt is definitely inside the callback.
  while (calls.load(std::memory_order_acquire) == 0) {
    std::this_thread::yield();
  }
  // A second caller gets nothing rather than starting its own attempt.
  CHECK(coordinator.attempt("42").empty());
  CHECK_EQ(calls.load(), 1);

  release.store(true, std::memory_order_release);
  first.join();

  // Once the first finished, re-discovery is available again.
  release.store(true, std::memory_order_release);
  CHECK(!coordinator.attempt("42").empty());
  CHECK_EQ(calls.load(), 2);
}

void testTheDefaultThresholdMatchesCrowdyJs() {
  // CrowdyJS's rediscoverAfterFailures default. Moving on the first failure
  // would relocate clients across a datacenter on any transient loss.
  CHECK_EQ(RediscoverCoordinator::kDefaultAfterFailures, 3);
}

}  // namespace

int main() {
  testNoCallbackMeansNoAnswer();
  testTheAnswerIsPassedThrough();
  testAnEmptyAnswerIsNotAMove();
#ifndef CROWDY_NO_EXCEPTIONS
  testAThrowingCallbackIsNotFatal();
#endif
  testConcurrentAttemptsShareOne();
  testTheDefaultThresholdMatchesCrowdyJs();
  std::puts("rediscover_test OK");
  return 0;
}
