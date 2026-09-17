#include "CrowdyNative.hpp"

#include <crowdy/crowdy.hpp>

#include <sstream>

using namespace godot;

String CrowdyNative::login(
	const String& email,
	const String& password
) {
	try {
		auto auth =
			identityClient->auth().login(
				(std::string)email.utf8().ptr(),
				(std::string)password.utf8().ptr()
			);

		std::ostringstream out;

		out
			<< "{\"ok\":true,\"token\":\""
			<< auth.token
			<< "\",\"user\":{\"userId\":\""
			<< auth.userId
			<< "\",\"email\":\""
			<< auth.email.valueOrEmpty()
			<< "\",\"gamertag\":\""
			<< auth.gamertag.valueOrEmpty()
			<< "\"}}";

		return String(out.str().c_str());
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		return String(err.c_str());
	}
}

void CrowdyNative::login_async(
	const String& email,
	const String& password,
	const Callable& cb
) {
	try {
		identityClient->auth().loginAsync(
			(std::string)email.utf8().ptr(),
			(std::string)password.utf8().ptr(),

			[this, cb](
				crowdy::graphql::GraphQLOutcome out,
				crowdy::domains::AuthResponse auth
				) mutable {
					std::ostringstream result;

					if (out.ok()) {
						result
							<< "{\"ok\":true,\"token\":\""
							<< auth.token
							<< "\",\"user\":{\"userId\":\""
							<< auth.userId
							<< "\",\"email\":\""
							<< auth.email.valueOrEmpty()
							<< "\",\"gamertag\":\""
							<< auth.gamertag.valueOrEmpty()
							<< "\"}}";
					}
					else {
						result << "{\"ok\":false}";
					}

					this->call_deferred(
						"_invoke_callable",
						Callable(cb),
						String(result.str().c_str())
					);
			}
		);
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		call_deferred(
			"_invoke_callable",
			Callable(cb),
			String(err.c_str())
		);
	}
}

String CrowdyNative::register_user(
	const String& email,
	const String& password,
	const String& gamertag
) {
	try {
		auto auth =
			identityClient->auth().registerUser(
				(std::string)email.utf8().ptr(),
				(std::string)password.utf8().ptr(),
				(std::string)gamertag.utf8().ptr()
			);

		std::ostringstream out;

		out
			<< "{\"ok\":true,\"token\":\""
			<< auth.token
			<< "\",\"user\":{\"userId\":\""
			<< auth.userId
			<< "\",\"email\":\""
			<< auth.email.valueOrEmpty()
			<< "\",\"gamertag\":\""
			<< auth.gamertag.valueOrEmpty()
			<< "\"}}";

		return String(out.str().c_str());
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		return String(err.c_str());
	}
}

void CrowdyNative::register_user_async(
	const String& email,
	const String& password,
	const String& gamertag,
	const Callable& cb
) {
	try {
		identityClient->auth().registerUserAsync(
			(std::string)email.utf8().ptr(),
			(std::string)password.utf8().ptr(),
			(std::string)gamertag.utf8().ptr(),

			[this, cb](
				crowdy::graphql::GraphQLOutcome out,
				crowdy::domains::AuthResponse auth
				) mutable {
					std::ostringstream result;

					if (out.ok()) {
						result
							<< "{\"ok\":true,\"token\":\""
							<< auth.token
							<< "\",\"user\":{\"userId\":\""
							<< auth.userId
							<< "\",\"email\":\""
							<< auth.email.valueOrEmpty()
							<< "\",\"gamertag\":\""
							<< auth.gamertag.valueOrEmpty()
							<< "\"}}";
					}
					else {
						result << "{\"ok\":false}";
					}

					this->call_deferred(
						"_invoke_callable",
						Callable(cb),
						String(result.str().c_str())
					);
			}
		);
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		call_deferred(
			"_invoke_callable",
			Callable(cb),
			String(err.c_str())
		);
	}
}

String CrowdyNative::logout() {
	try {
		bool ok =
			identityClient->auth().logout();

		std::ostringstream out;

		out
			<< "{\"ok\":true,\"result\":"
			<< (ok ? "true" : "false")
			<< "}";

		return String(out.str().c_str());
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		return String(err.c_str());
	}
}

void CrowdyNative::logout_async(
	const Callable& cb
) {
	try {
		identityClient->auth().logoutAsync(
			[this, cb](
				crowdy::graphql::GraphQLOutcome out,
				bool ok
				) mutable {
					std::ostringstream result;

					result
						<< "{\"ok\":"
						<< (out.ok() ? "true" : "false")
						<< ",\"result\":"
						<< (ok ? "true" : "false")
						<< "}";

					this->call_deferred(
						"_invoke_callable",
						Callable(cb),
						String(result.str().c_str())
					);
			}
		);
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		call_deferred(
			"_invoke_callable",
			Callable(cb),
			String(err.c_str())
		);
	}
}

String CrowdyNative::graphql_query(
	const String& endpoint,
	const String& query
) {
	(void)endpoint;

	try {
		auto result =
			identityClient->graphqlClient().request(
				(std::string)query.utf8().ptr()
			);

		return String(result.dump().c_str());
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"error\":\"")
			+ e.what()
			+ "\"}";

		return String(err.c_str());
	}
}

String CrowdyNative::request_login_link(
	const String& email,
	const String& redirect_uri
) {
	try {
		auto response =
			identityClient->auth().requestLoginLink(
				(std::string)email.utf8().ptr(),
				(std::string)redirect_uri.utf8().ptr()
			);

		return String(response.dump().c_str());
	}
	catch (const std::exception& e) {
		std::string err =
			std::string("{\"ok\":false,\"error\":\"")
			+ e.what()
			+ "\"}";

		return String(err.c_str());
	}
}
