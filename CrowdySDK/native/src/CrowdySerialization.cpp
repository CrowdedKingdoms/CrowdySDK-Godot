#include "CrowdySerialization.hpp"

#include <crowdy/crowdy.hpp>
#include <crowdy/replication/connection.hpp>

#include <iomanip>
#include <sstream>

std::string actor_uuid_to_hex(
	const crowdy::core::ActorUuid& uuid
) {
	const unsigned char* data =
		reinterpret_cast<const unsigned char*>(uuid.data());

	std::ostringstream out;

	for (size_t i = 0; i < crowdy::wire::kUuidSize; ++i) {
		out
			<< std::hex
			<< std::setw(2)
			<< std::setfill('0')
			<< static_cast<int>(data[i]);
	}

	return out.str();
}

std::string chunk_to_json(
	const crowdy::wire::ChunkCoord& chunk
) {
	std::ostringstream out;

	out
		<< "{\"x\":" << chunk.x
		<< ",\"y\":" << chunk.y
		<< ",\"z\":" << chunk.z
		<< "}";

	return out.str();
}

std::string bytes_to_base64(
	const crowdy::Bytes& bytes
) {
	if (bytes.empty()) {
		return "\"\"";
	}

	auto encoded = crowdy::core::base64Encode(bytes);

	std::ostringstream out;
	out << '"' << encoded << '"';

	return out.str();
}

std::string spatial_notification_to_json(
	const crowdy::replication::SpatialNotification& notification
) {
	std::ostringstream out;

	out
		<< "{\"type\":\""
		<< static_cast<int>(notification.type)
		<< "\",";

	out
		<< "\"appId\":"
		<< notification.appId
		<< ",";

	out
		<< "\"chunk\":"
		<< chunk_to_json(notification.chunk)
		<< ",";

	out
		<< "\"uuid\":\""
		<< actor_uuid_to_hex(notification.uuidArray())
		<< "\",";

	out << "\"payload\":";

	if (!notification.payload.empty()) {
		std::string encoded =
			crowdy::core::base64Encode(notification.payload);

		out << '"' << encoded << '"';
	}
	else {
		out << "\"\"";
	}

	out
		<< ",\"epochMillis\":"
		<< notification.epochMillis;

	out
		<< ",\"sequence\":"
		<< static_cast<int>(notification.sequence);

	out << "}";

	return out.str();
}
