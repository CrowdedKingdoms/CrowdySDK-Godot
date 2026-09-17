#include <crowdy/crowdy.hpp>
#include <crowdy/graphql/http.hpp>

#include <memory>
#include <thread>

namespace {

	class ThreadedAsyncTransport final
		: public crowdy::graphql::IAsyncHttpTransport {

	public:
		explicit ThreadedAsyncTransport(
			std::shared_ptr<crowdy::graphql::IHttpTransport> sync
		)
			: sync_(std::move(sync)) {
		}

		void sendAsync(
			const crowdy::graphql::HttpRequest& request,
			std::function<void(crowdy::graphql::HttpOutcome)> cb
		) override {
			auto sync = sync_;

			std::thread(
				[sync, request, cb = std::move(cb)]() mutable {

					crowdy::graphql::HttpOutcome out;

#ifndef CROWDY_NO_EXCEPTIONS
					try {
						out.response = sync->send(request);
						out.status = crowdy::Errc::Ok;
					}
					catch (const crowdy::graphql::CrowdyTimeoutError& e) {
						out.status = crowdy::Errc::Timeout;
						out.errorMessage = e.what();
					}
					catch (const std::exception& e) {
						out.status = crowdy::Errc::SocketError;
						out.errorMessage = e.what();
					}
#else
					out.response = sync->send(request);
					out.status = crowdy::Errc::Ok;
#endif

					cb(std::move(out));
				}
			).detach();
		}

	private:
		std::shared_ptr<crowdy::graphql::IHttpTransport> sync_;
	};

} // namespace

std::shared_ptr<crowdy::graphql::IAsyncHttpTransport>
make_threaded_async_transport(
	const std::shared_ptr<crowdy::graphql::IHttpTransport>& sync
) {
	return std::make_shared<ThreadedAsyncTransport>(sync);
}
