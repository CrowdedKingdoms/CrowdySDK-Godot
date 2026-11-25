#pragma once

// Conversions between CrowdyCPP's types and Godot's, in one place so every class marshals
// the same way: text is UTF-8, binary payloads stay binary (PackedByteArray, never base64),
// actor ids are the 32-character strings the Replication API uses, and errors are named with
// stable SCREAMING_SNAKE_CASE codes that GDScript can compare against.

#include <crowdy/core/bytes.hpp>
#include <crowdy/core/result.hpp>
#include <crowdy/core/uuid.hpp>
#include <crowdy/wire/codec.hpp>
#include <crowdy/wire/protocol.hpp>

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <string>
#include <string_view>

namespace crowdy_godot {

godot::String to_godot(std::string_view p_text);
std::string to_std(const godot::String &p_text);

godot::PackedByteArray to_packed_bytes(crowdy::Bytes p_bytes);
// The span is valid only while p_bytes is alive and unmodified.
crowdy::Bytes as_bytes(const godot::PackedByteArray &p_bytes);

// Godot's Vector3i is 32-bit; chunk coordinates beyond that range are not supported.
godot::Vector3i to_vector3i(const crowdy::wire::ChunkCoord &p_chunk);
crowdy::wire::ChunkCoord to_chunk(const godot::Vector3i &p_chunk);

godot::String to_actor_id(const crowdy::core::ActorUuid &p_uuid);
// Notifications carry the actor uuid as 32 bytes that are not null-terminated.
godot::String to_actor_id(const char *p_uuid);
bool parse_actor_id(const godot::String &p_actor_id, crowdy::core::ActorUuid &r_uuid);

const char *error_code_of(crowdy::Errc p_code);
const char *error_code_of(crowdy::wire::ErrorCode p_code);

// For the synchronous send methods, which return godot::Error like Godot's own network peers.
godot::Error to_godot_error(crowdy::Errc p_code);

} // namespace crowdy_godot
