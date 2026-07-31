#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cuac_test {

enum class ControlledSocketMode {
	SUCCESS,
	AUTHENTICATED_SUCCESS,
	AUTHENTICATED_SET_COOKIE,
	GRAPHQL_SUCCESS,
	SET_COOKIE,
	STATUS,
	AUTHENTICATION_STATUS,
	AUTHORIZATION_STATUS,
	LINK_SUCCESS,
	MANY_LINK_SUCCESS,
	INTERIM_LINK_SUCCESS,
	TRAILER_LINK_SUCCESS,
	LINK_STATUS,
	PAGINATED_REPOSITORIES,
	REDIRECT,
	MALFORMED,
	OVERSIZED_HEADER,
	OVERSIZED_RESPONSE,
	OVERSIZED_CHUNK_FRAMING,
	MALFORMED_CHUNK_EXTENSION,
	CHUNKED_SUCCESS,
	GZIP_EXACT_DECOMPRESSED_LIMIT,
	GZIP_OVER_DECOMPRESSED_LIMIT,
	DISCONNECT,
	BLOCK,
	PARTIAL_RESPONSE_BLOCK,
	BLOCK_THEN_AUTHENTICATED_SUCCESS
};

// Bounded private HTTP/1.1 service. The service owns no product
// authority and is linked only into focused Runtime tests.
class ControlledSocketService {
public:
	explicit ControlledSocketService(ControlledSocketMode mode, uint16_t redirect_port = 0);
	~ControlledSocketService() noexcept;

	uint16_t Port() const noexcept;
	bool WaitForRequest(std::chrono::milliseconds timeout);
	bool WaitForRequestCount(uint64_t count, std::chrono::milliseconds timeout);
	std::string Request() const;
	std::vector<std::string> Requests() const;
	uint64_t ConnectionCount() const noexcept;
	uint64_t ResponseBodyBytes() const noexcept;

private:
	ControlledSocketService(const ControlledSocketService &) = delete;
	ControlledSocketService &operator=(const ControlledSocketService &) = delete;

	static bool SendAll(int socket_fd, const std::string &bytes) noexcept;
	std::string Response(ControlledSocketMode response_mode) const;
	std::string PaginatedResponse(uint64_t page_index) const;
	void Serve() noexcept;

	const ControlledSocketMode mode;
	const uint16_t redirect_port;
	int listener;
	std::atomic<int> client;
	uint16_t port;
	mutable std::mutex mutex;
	std::condition_variable condition;
	std::thread worker;
	std::string request;
	std::vector<std::string> requests;
	bool request_ready;
	bool stop;
	std::atomic<uint64_t> connection_count;
};

} // namespace cuac_test
