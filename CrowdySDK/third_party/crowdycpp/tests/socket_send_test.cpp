// Classification of send errors, tested without a socket so it runs on every
// platform. That matters most on Windows: the loopback replication test is not
// built there, and WSAEWOULDBLOCK on a non-blocking Winsock socket is the
// condition that used to be reported as a dead socket.
//
// A full kernel send buffer is not a failure. Anything that folds it in with
// real faults leaves a consumer unable to decide between retrying and
// reconnecting, which is the defect this file guards.
#include <string>

#include "replication/socket_errors.hpp"
#include "test_util.hpp"

using namespace crowdy;
using crowdy::replication::detail::sendErrcFromNativeError;

namespace {

void run() {
  // --- Transient backpressure: nothing was sent, the socket is healthy.
#ifdef _WIN32
  CHECK_EQ(sendErrcFromNativeError(WSAEWOULDBLOCK), Errc::WouldBlock);
  CHECK_EQ(sendErrcFromNativeError(WSAENOBUFS), Errc::WouldBlock);
#else
  CHECK_EQ(sendErrcFromNativeError(EAGAIN), Errc::WouldBlock);
  CHECK_EQ(sendErrcFromNativeError(EWOULDBLOCK), Errc::WouldBlock);
  // Linux picks between EAGAIN and ENOBUFS by datagram size, not by anything
  // the caller did differently, so treating only EAGAIN as backpressure still
  // reports ordinary saturation as a fault for larger datagrams.
  CHECK_EQ(sendErrcFromNativeError(ENOBUFS), Errc::WouldBlock);
#endif

  // --- Genuine faults stay faults. Without these the mapping could be made
  // to pass by returning WouldBlock for everything, which would be worse than
  // the bug: a dead socket would look like a busy one, forever.
#ifdef _WIN32
  CHECK_EQ(sendErrcFromNativeError(WSAENOTSOCK), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAECONNRESET), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAECONNREFUSED), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAEMSGSIZE), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAENETDOWN), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAENETUNREACH), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(WSAESHUTDOWN), Errc::SocketError);
#else
  CHECK_EQ(sendErrcFromNativeError(ECONNREFUSED), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(EBADF), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(ENOTSOCK), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(EMSGSIZE), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(EPIPE), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(EACCES), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(ENETUNREACH), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(EINVAL), Errc::SocketError);
#endif

  // An unrecognised code is a fault, not backpressure: guessing "retry" for an
  // error we have never seen would hide it behind an unbounded requeue.
  CHECK_EQ(sendErrcFromNativeError(0), Errc::SocketError);
  CHECK_EQ(sendErrcFromNativeError(999999), Errc::SocketError);

  // --- The two outcomes must remain distinguishable to a consumer.
  CHECK(Errc::WouldBlock != Errc::SocketError);
  CHECK_EQ(std::string(errcName(Errc::WouldBlock)), std::string("WouldBlock"));
  CHECK_EQ(std::string(errcName(Errc::SocketError)), std::string("SocketError"));

  // WouldBlock was appended, so every previously assigned value is unmoved.
  // Nothing serialises these ordinals today; this keeps that decision honest
  // if someone later inserts an enumerator in the middle.
  CHECK_EQ(static_cast<int>(Errc::Ok), 0);
  CHECK_EQ(static_cast<int>(Errc::SocketError), 6);
  CHECK_EQ(static_cast<int>(Errc::CryptoUnavailable), 11);
  CHECK_EQ(static_cast<int>(Errc::WouldBlock), 12);
}

}  // namespace

int main() {
  run();
  std::puts("socket_send_test OK");
  return 0;
}
