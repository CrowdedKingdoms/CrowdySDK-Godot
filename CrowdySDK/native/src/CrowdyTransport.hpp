#pragma once

#include <crowdy/crowdy.hpp>

#include <memory>

std::shared_ptr<crowdy::graphql::IAsyncHttpTransport>
make_threaded_async_transport(
	const std::shared_ptr<crowdy::graphql::IHttpTransport>& sync
);
