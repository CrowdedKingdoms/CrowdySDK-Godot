#pragma once

#include <crowdy/crowdy.hpp>

#include <string>

std::string actor_uuid_to_hex(
	const crowdy::core::ActorUuid& uuid
);

std::string chunk_to_json(
	const crowdy::wire::ChunkCoord& chunk
);

std::string bytes_to_base64(
	const crowdy::Bytes& bytes
);

std::string spatial_notification_to_json(
	const crowdy::replication::SpatialNotification& notification
);
