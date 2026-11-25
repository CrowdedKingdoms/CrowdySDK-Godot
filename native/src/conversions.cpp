#include "conversions.h"

#include <cstring>

using namespace godot;

namespace crowdy_godot {

String to_godot(std::string_view p_text) {
	return String::utf8(p_text.data(), static_cast<int64_t>(p_text.size()));
}

std::string to_std(const String &p_text) {
	const CharString utf8 = p_text.utf8();
	return std::string(utf8.get_data(), static_cast<size_t>(utf8.length()));
}

PackedByteArray to_packed_bytes(crowdy::Bytes p_bytes) {
	PackedByteArray packed;
	packed.resize(static_cast<int64_t>(p_bytes.size()));
	if (!p_bytes.empty()) {
		std::memcpy(packed.ptrw(), p_bytes.data(), p_bytes.size());
	}
	return packed;
}

crowdy::Bytes as_bytes(const PackedByteArray &p_bytes) {
	return crowdy::Bytes(p_bytes.ptr(), static_cast<size_t>(p_bytes.size()));
}

Vector3i to_vector3i(const crowdy::wire::ChunkCoord &p_chunk) {
	return Vector3i(static_cast<int32_t>(p_chunk.x), static_cast<int32_t>(p_chunk.y), static_cast<int32_t>(p_chunk.z));
}

crowdy::wire::ChunkCoord to_chunk(const Vector3i &p_chunk) {
	return crowdy::wire::ChunkCoord{ p_chunk.x, p_chunk.y, p_chunk.z };
}

String to_actor_id(const crowdy::core::ActorUuid &p_uuid) {
	return to_godot(std::string_view(p_uuid.data(), p_uuid.size()));
}

String to_actor_id(const char *p_uuid) {
	return to_godot(std::string_view(p_uuid, crowdy::wire::kUuidSize));
}

bool parse_actor_id(const String &p_actor_id, crowdy::core::ActorUuid &r_uuid) {
	return crowdy::core::actorUuidFromString(to_std(p_actor_id), r_uuid);
}

const char *error_code_of(crowdy::Errc p_code) {
	switch (p_code) {
		case crowdy::Errc::Ok:
			return "OK";
		case crowdy::Errc::InvalidArgument:
			return "INVALID_ARGUMENT";
		case crowdy::Errc::BufferTooSmall:
			return "BUFFER_TOO_SMALL";
		case crowdy::Errc::Malformed:
			return "MALFORMED";
		case crowdy::Errc::HmacMismatch:
			return "HMAC_MISMATCH";
		case crowdy::Errc::NotConnected:
			return "NOT_CONNECTED";
		case crowdy::Errc::SocketError:
			return "SOCKET_ERROR";
		case crowdy::Errc::Timeout:
			return "TIMEOUT";
		case crowdy::Errc::TokenExpired:
			return "TOKEN_EXPIRED";
		case crowdy::Errc::Rejected:
			return "REJECTED";
		case crowdy::Errc::Closed:
			return "CLOSED";
		case crowdy::Errc::CryptoUnavailable:
			return "CRYPTO_UNAVAILABLE";
		case crowdy::Errc::WouldBlock:
			return "WOULD_BLOCK";
		case crowdy::Errc::Cancelled:
			return "CANCELLED";
		case crowdy::Errc::ResponseTooLarge:
			return "RESPONSE_TOO_LARGE";
	}
	return "UNKNOWN_ERROR";
}

const char *error_code_of(crowdy::wire::ErrorCode p_code) {
	using crowdy::wire::ErrorCode;
	switch (p_code) {
		case ErrorCode::NoError:
			return "OK";
		case ErrorCode::UnknownError:
			return "UNKNOWN_ERROR";
		case ErrorCode::InvalidToken:
			return "INVALID_TOKEN";
		case ErrorCode::AppNotFound:
			return "APP_NOT_FOUND";
		case ErrorCode::Unauthorized:
			return "UNAUTHORIZED";
		case ErrorCode::GameTokenWrongSize:
			return "GAME_TOKEN_WRONG_SIZE";
		case ErrorCode::InvalidRequest:
			return "INVALID_REQUEST";
		case ErrorCode::InvalidAppId:
			return "INVALID_APP_ID";
		case ErrorCode::UserNotAuthenticated:
			return "USER_NOT_AUTHENTICATED";
		case ErrorCode::TokenExpired:
			return "TOKEN_EXPIRED";
		case ErrorCode::AppPaused:
			return "APP_PAUSED";
	}
	// A code added by a newer replication server.
	return "UNKNOWN_ERROR";
}

Error to_godot_error(crowdy::Errc p_code) {
	switch (p_code) {
		case crowdy::Errc::Ok:
			return OK;
		case crowdy::Errc::InvalidArgument:
		case crowdy::Errc::BufferTooSmall:
		case crowdy::Errc::Malformed:
			return ERR_INVALID_PARAMETER;
		case crowdy::Errc::WouldBlock:
			return ERR_BUSY;
		case crowdy::Errc::NotConnected:
		case crowdy::Errc::Closed:
		case crowdy::Errc::SocketError:
			return ERR_CONNECTION_ERROR;
		case crowdy::Errc::Timeout:
			return ERR_TIMEOUT;
		case crowdy::Errc::TokenExpired:
			return ERR_UNAUTHORIZED;
		case crowdy::Errc::CryptoUnavailable:
			return ERR_UNAVAILABLE;
		case crowdy::Errc::HmacMismatch:
		case crowdy::Errc::ResponseTooLarge:
			return ERR_INVALID_DATA;
		case crowdy::Errc::Rejected:
		case crowdy::Errc::Cancelled:
			return FAILED;
	}
	return FAILED;
}

} // namespace crowdy_godot
