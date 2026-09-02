// Replays CrowdyJS's test/unit/reconnect-directive.test.mjs vector for vector.
// Both SDKs bound endpoint moves the same way or one of them can be walked
// somewhere the other cannot, and that difference would only ever show up in
// production, on whichever SDK is more permissive.
#include "crowdy/graphql/auth_state.hpp"
#include "crowdy/graphql/estate.hpp"
#include "test_util.hpp"

using crowdy::graphql::estateHostname;
using crowdy::graphql::isSameEstate;

namespace {

void testAcceptsSiblingInstanceInSameEstate() {
  CHECK(isSameEstate("wss://ck-api-1.pgc.prod.cp.cks-env.com/realtime",
                     "wss://ck-api-4.pgc.prod.cp.cks-env.com"));
}

void testAcceptsSharedLoadBalancer() {
  CHECK(isSameEstate("wss://ck-api-1.pgc.prod.cp.cks-env.com/realtime",
                     "wss://ck.prod.cp.cks-env.com"));
}

void testAcceptsIdenticalOrigin() {
  CHECK(isSameEstate("wss://ck.example.com/realtime", "wss://ck.example.com"));
}

void testRefusesOriginOutsideEstate() {
  CHECK(!isSameEstate("wss://ck-api-1.pgc.prod.cp.cks-env.com/realtime",
                      "wss://evil.example.com"));
}

void testRefusesLookalikeSharingOnlyAPrefix() {
  // cks-env.com.evil.com must not pass as cks-env.com.
  CHECK(!isSameEstate("wss://ck.prod.cp.cks-env.com/realtime",
                      "wss://ck.prod.cp.cks-env.com.evil.com"));
}

void testRefusesUnparseableRatherThanGuessing() {
  CHECK(!isSameEstate("wss://ck.example.com", "not a url"));
  CHECK(!isSameEstate("not a url", "wss://ck.example.com"));
  CHECK(!isSameEstate("", ""));
}

void testBareHostnameMatchesOnlyItself() {
  CHECK(isSameEstate("wss://localhost/realtime", "wss://localhost"));
  CHECK(!isSameEstate("wss://localhost/realtime", "wss://otherhost"));
}

void testHostnameParsing() {
  CHECK_EQ(estateHostname("https://CK.Example.COM/graphql").value_or(""),
           "ck.example.com");
  CHECK_EQ(estateHostname("wss://ck.example.com:8443/graphql").value_or(""),
           "ck.example.com");
  CHECK_EQ(estateHostname("https://ck.example.com?a=b").value_or(""),
           "ck.example.com");
  CHECK_EQ(estateHostname("https://[2001:db8::1]:443/graphql").value_or(""),
           "[2001:db8::1]");
  CHECK(!estateHostname("https://").has_value());
  CHECK(!estateHostname("ck.example.com").has_value());

  // Userinfo must not be mistaken for the host, in either direction.
  CHECK_EQ(estateHostname("wss://evil.com@ck.example.com/x").value_or(""),
           "ck.example.com");
  CHECK(!isSameEstate("wss://ck.prod.cp.cks-env.com",
                      "wss://cks-env.com@evil.example.com"));
}

// A localhost pair is the shape local development is in, and a two-instance
// estate is the shape a fresh deployment is in. Both are worth stating as
// deliberate outcomes rather than leaving as whatever the rule happens to do.
void testDevelopmentAndSingleInstanceShapes() {
  CHECK(isSameEstate("http://localhost:3000/graphql",
                     "ws://localhost:3000/graphql"));
  CHECK(!isSameEstate("http://localhost:3000", "http://127.0.0.1:3000"));
  CHECK(isSameEstate("https://ck-or.prod.cp.cks-env.com",
                     "https://ck-va.prod.cp.cks-env.com"));
}

// The token-store naming split lives here because it enforces the same kind of
// bound: which credential may be seen from where.
void testTokenPathsKeepTheTwoCredentialsApart() {
  using crowdy::graphql::FileTokenStore;

  // One session per ORIGIN. Two games on the same origin must resolve the same
  // path, or a player who signed in for one is anonymous to the next.
  const std::string a =
      FileTokenStore::sessionPath("/var/lib/crowdy", "https://ck.example.com");
  const std::string b =
      FileTokenStore::sessionPath("/var/lib/crowdy/", "ck.example.com");
  CHECK_EQ(a, b);
  CHECK_EQ(a, "/var/lib/crowdy/crowdy-session-ck.example.com");

  // Different origins do NOT share a session.
  CHECK(a != FileTokenStore::sessionPath("/var/lib/crowdy",
                                         "https://ck.other.com"));

  // App tokens are per app, because each authorises exactly one.
  CHECK_EQ(FileTokenStore::appPath("/var/lib/crowdy", "77330192913920"),
           "/var/lib/crowdy/crowdy-app-77330192913920");
  CHECK(FileTokenStore::appPath("/var/lib/crowdy", "1") !=
        FileTokenStore::appPath("/var/lib/crowdy", "2"));

  // A session path is never an app path, whatever the inputs.
  CHECK(FileTokenStore::sessionPath("/d", "42") !=
        FileTokenStore::appPath("/d", "42"));

  // A hostile origin must not escape the directory. What makes traversal
  // possible is the SEPARATOR, not the dots: `..` with no `/` after it is just
  // an odd filename, so separators are what must not survive.
  const std::string escaped =
      FileTokenStore::sessionPath("/d", "https://evil/../../etc/passwd");
  CHECK_EQ(escaped, "/d/crowdy-session-evil_.._.._etc_passwd");
  CHECK_EQ(escaped.find('/', 3), std::string::npos);
  CHECK_EQ(escaped.find('\\'), std::string::npos);
}

}  // namespace

int main() {
  testTokenPathsKeepTheTwoCredentialsApart();
  testAcceptsSiblingInstanceInSameEstate();
  testAcceptsSharedLoadBalancer();
  testAcceptsIdenticalOrigin();
  testRefusesOriginOutsideEstate();
  testRefusesLookalikeSharingOnlyAPrefix();
  testRefusesUnparseableRatherThanGuessing();
  testBareHostnameMatchesOnlyItself();
  testHostnameParsing();
  testDevelopmentAndSingleInstanceShapes();
  std::puts("estate_test OK");
  return 0;
}
