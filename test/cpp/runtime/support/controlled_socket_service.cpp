#include "runtime/support/controlled_socket_service.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace cuac_test {
namespace {

const unsigned char GZIP_EXACT[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xca, 0x31, 0x0e, 0x82, 0x40, 0x14, 0x40,
    0xc1, 0xab, 0x98, 0x5f, 0xd3, 0xd0, 0xee, 0x55, 0x8c, 0x31, 0xc0, 0xa2, 0xd9, 0x00, 0x52, 0x2c, 0x54, 0x84,
    0xbb, 0xa3, 0xd7, 0x30, 0x33, 0xe5, 0xcb, 0x3b, 0xa2, 0x6c, 0xe3, 0x52, 0x23, 0xdd, 0x8f, 0x28, 0x39, 0x52,
    0xdb, 0x36, 0x31, 0xaf, 0xef, 0xf2, 0x89, 0x14, 0x79, 0x1f, 0xa6, 0xdc, 0x47, 0x13, 0xf5, 0xfb, 0x3c, 0xbb,
    0xbc, 0xfc, 0xea, 0xab, 0x9b, 0xeb, 0x78, 0x3e, 0xce, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xff, 0xe4, 0x02, 0xd4, 0xf9, 0x4b, 0x19, 0x00, 0x00, 0x01, 0x00};

const unsigned char GZIP_OVER[] = {
    0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0xff, 0xed, 0xca, 0x31, 0x0e, 0x82, 0x40, 0x14, 0x40,
    0xc1, 0xab, 0x98, 0x5f, 0xd3, 0xd0, 0xee, 0x55, 0x8c, 0x31, 0xc0, 0xa2, 0xd9, 0x00, 0x52, 0x2c, 0x54, 0x84,
    0xbb, 0xa3, 0xd7, 0x30, 0x33, 0xe5, 0xcb, 0x3b, 0xa2, 0x6c, 0xe3, 0x52, 0x23, 0xdd, 0x8f, 0x28, 0x39, 0x52,
    0xdb, 0x36, 0x31, 0xaf, 0xef, 0xf2, 0x89, 0x14, 0x79, 0x1f, 0xa6, 0xdc, 0x47, 0x13, 0xf5, 0xfb, 0x3c, 0xbb,
    0xbc, 0xfc, 0xea, 0xab, 0x9b, 0xeb, 0x78, 0x3e, 0xce, 0x1b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x7f, 0xe5, 0x02, 0x71, 0x92, 0xcb, 0x68, 0x01, 0x00, 0x01, 0x00};

std::string Binary(const unsigned char *bytes, std::size_t size) {
	return std::string(reinterpret_cast<const char *>(bytes), size);
}

bool RequestComplete(const std::string &request) {
	const auto header_end = request.find("\r\n\r\n");
	if (header_end == std::string::npos) {
		return false;
	}
	const std::string marker = "\r\nContent-Length: ";
	const auto begin = request.find(marker);
	if (begin == std::string::npos || begin >= header_end) {
		return true;
	}
	const auto digits = begin + marker.size();
	const auto line_end = request.find("\r\n", digits);
	if (line_end == std::string::npos || line_end > header_end || line_end == digits) {
		return true;
	}
	std::size_t body_bytes = 0;
	for (std::size_t index = digits; index < line_end; index++) {
		if (request[index] < '0' || request[index] > '9') {
			return true;
		}
		const auto digit = static_cast<std::size_t>(request[index] - '0');
		if (body_bytes > (65536 - digit) / 10) {
			return true;
		}
		body_bytes = body_bytes * 10 + digit;
	}
	return request.size() >= header_end + 4 + body_bytes;
}

} // namespace

ControlledSocketService::ControlledSocketService(ControlledSocketMode mode_p, uint16_t redirect_port_p)
    : mode(mode_p), redirect_port(redirect_port_p), listener(-1), client(-1), port(0), request_ready(false),
      stop(false), connection_count(0) {
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) {
		throw std::runtime_error("controlled service socket failed");
	}
	int enabled = 1;
	(void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
	sockaddr_in address;
	std::memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) != 0 ||
	    listen(listener, 1) != 0) {
		close(listener);
		throw std::runtime_error("controlled service bind failed");
	}
	socklen_t address_length = sizeof(address);
	if (getsockname(listener, reinterpret_cast<sockaddr *>(&address), &address_length) != 0) {
		close(listener);
		throw std::runtime_error("controlled service address failed");
	}
	port = ntohs(address.sin_port);
	worker = std::thread(&ControlledSocketService::Serve, this);
}

ControlledSocketService::~ControlledSocketService() noexcept {
	{
		std::lock_guard<std::mutex> guard(mutex);
		stop = true;
	}
	condition.notify_all();
	if (client.load(std::memory_order_acquire) >= 0) {
		(void)shutdown(client.load(std::memory_order_relaxed), SHUT_RDWR);
	}
	(void)shutdown(listener, SHUT_RDWR);
	close(listener);
	listener = -1;
	if (worker.joinable()) {
		worker.join();
	}
}

uint16_t ControlledSocketService::Port() const noexcept {
	return port;
}

bool ControlledSocketService::WaitForRequest(std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> guard(mutex);
	return condition.wait_for(guard, timeout, [&]() { return request_ready; });
}

bool ControlledSocketService::WaitForRequestCount(uint64_t count, std::chrono::milliseconds timeout) {
	std::unique_lock<std::mutex> guard(mutex);
	return condition.wait_for(guard, timeout, [&]() { return requests.size() >= count; });
}

std::string ControlledSocketService::Request() const {
	std::lock_guard<std::mutex> guard(mutex);
	return request;
}

std::vector<std::string> ControlledSocketService::Requests() const {
	std::lock_guard<std::mutex> guard(mutex);
	return requests;
}

uint64_t ControlledSocketService::ConnectionCount() const noexcept {
	return connection_count.load(std::memory_order_relaxed);
}

uint64_t ControlledSocketService::ResponseBodyBytes() const noexcept {
	if (mode == ControlledSocketMode::GZIP_EXACT_DECOMPRESSED_LIMIT) {
		return sizeof(GZIP_EXACT);
	}
	if (mode == ControlledSocketMode::GZIP_OVER_DECOMPRESSED_LIMIT) {
		return sizeof(GZIP_OVER);
	}
	return 0;
}

bool ControlledSocketService::SendAll(int socket_fd, const std::string &bytes) noexcept {
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto sent = send(socket_fd, bytes.data() + offset, bytes.size() - offset, 0);
		if (sent <= 0) {
			return false;
		}
		offset += static_cast<std::size_t>(sent);
	}
	return true;
}

std::string ControlledSocketService::Response(ControlledSocketMode response_mode) const {
	const std::string success_body = "{\"items\":[{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false},"
	                                 "{\"id\":22,\"login\":\"cuac\",\"site_admin\":true},"
	                                 "{\"id\":33,\"login\":\"three\",\"site_admin\":false}]}";
	const std::string authenticated_body = "{\"id\":44,\"login\":\"authenticated\",\"site_admin\":false}";
	const std::string graphql_body =
	    "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[{\"id\":\"R44\","
	    "\"nameWithOwner\":\"owner/graphql\",\"owner\":{\"login\":\"owner\"},\"stargazerCount\":44,"
	    "\"primaryLanguage\":{\"name\":\"C++\"},\"isPrivate\":false,\"isArchived\":false,"
	    "\"updatedAt\":\"2026-01-01T00:00:00Z\"}],\"pageInfo\":{\"hasNextPage\":false,"
	    "\"endCursor\":null}}}},\"errors\":[]}";
	if (response_mode == ControlledSocketMode::GRAPHQL_SUCCESS) {
		return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
		       std::to_string(graphql_body.size()) + "\r\nConnection: close\r\n\r\n" + graphql_body;
	}
	if (response_mode == ControlledSocketMode::AUTHENTICATED_SUCCESS ||
	    response_mode == ControlledSocketMode::AUTHENTICATED_SET_COOKIE) {
		const std::string cookie_header = response_mode == ControlledSocketMode::AUTHENTICATED_SET_COOKIE
		                                      ? "Set-Cookie: runtime_cookie=AUTH_COOKIE_CANARY; Path=/\r\n"
		                                      : "";
		return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
		       std::to_string(authenticated_body.size()) + "\r\n" + cookie_header + "Connection: close\r\n\r\n" +
		       authenticated_body;
	}
	if (response_mode == ControlledSocketMode::SUCCESS || response_mode == ControlledSocketMode::SET_COOKIE) {
		const std::string cookie_header = response_mode == ControlledSocketMode::SET_COOKIE
		                                      ? "Set-Cookie: runtime_cookie=CONTROLLED_COOKIE_SECRET; Path=/\r\n"
		                                      : "";
		return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
		       std::to_string(success_body.size()) + "\r\n" + cookie_header + "Connection: close\r\n\r\n" +
		       success_body;
	}
	if (response_mode == ControlledSocketMode::LINK_SUCCESS) {
		const std::string previous = "<https://api.github.com/user/repos?per_page=100&page=1>; rel=prev";
		const std::string next = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\"";
		return "HTTP/1.1 200 OK\r\nLink: " + previous + "\r\nlInK:\t" + next +
		       "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(success_body.size()) +
		       "\r\nConnection: close\r\n\r\n" + success_body;
	}
	if (response_mode == ControlledSocketMode::MANY_LINK_SUCCESS) {
		std::string headers;
		for (std::size_t index = 0; index < 40; index++) {
			headers += "Link: <https://example.test/" + std::to_string(index) + ">; rel=prev\r\n";
		}
		return "HTTP/1.1 200 OK\r\n" + headers +
		       "Content-Type: application/json\r\nContent-Length: " + std::to_string(success_body.size()) +
		       "\r\nConnection: close\r\n\r\n" + success_body;
	}
	if (response_mode == ControlledSocketMode::INTERIM_LINK_SUCCESS) {
		const std::string interim = "<https://credential-canary.invalid/user/repos?per_page=100&page=2>; rel=next";
		const std::string second_interim =
		    "<https://second-credential-canary.invalid/user/repos?per_page=100&page=2>; rel=next";
		const std::string terminal = "<https://api.github.com/user/repos?per_page=100&page=2>; rel=next";
		return "HTTP/1.1 100 Continue\r\nLink: " + interim + "\r\nLink: " + second_interim +
		       "\r\n\r\nHTTP/1.1 200 OK\r\nLink: " + terminal +
		       "\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(success_body.size()) +
		       "\r\nConnection: close\r\n\r\n" + success_body;
	}
	if (response_mode == ControlledSocketMode::TRAILER_LINK_SUCCESS) {
		const std::string trailer = "Link: <https://api.github.com/user/repos?per_page=100&page=2>; rel=next\r\n";
		return "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n"
		       "Trailer: Link\r\nConnection: close\r\n\r\n2\r\n{}\r\n0\r\n" +
		       trailer + "\r\n";
	}
	if (response_mode == ControlledSocketMode::LINK_STATUS) {
		const std::string body = "LINK_STATUS_PRIVATE_BODY";
		return "HTTP/1.1 503 Service Unavailable\r\nLink: <https://credential-canary.invalid/>; "
		       "rel=next\r\nContent-Length: " +
		       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	}
	if (response_mode == ControlledSocketMode::STATUS) {
		const std::string body = "SECRET_STATUS_BODY http://127.0.0.1/private";
		return "HTTP/1.1 503 Service Unavailable\r\nContent-Length: " + std::to_string(body.size()) +
		       "\r\nConnection: close\r\n\r\n" + body;
	}
	if (response_mode == ControlledSocketMode::AUTHENTICATION_STATUS ||
	    response_mode == ControlledSocketMode::AUTHORIZATION_STATUS) {
		const auto authentication = response_mode == ControlledSocketMode::AUTHENTICATION_STATUS;
		const std::string body = authentication ? "AUTHENTICATION_RESPONSE_CANARY" : "AUTHORIZATION_RESPONSE_CANARY";
		return std::string("HTTP/1.1 ") + (authentication ? "401 Unauthorized" : "403 Forbidden") +
		       "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	}
	if (response_mode == ControlledSocketMode::REDIRECT) {
		const auto destination_port = redirect_port == 0 ? port : redirect_port;
		return "HTTP/1.1 302 Found\r\nLocation: http://127.0.0.1:" + std::to_string(destination_port) +
		       "/forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	}
	if (response_mode == ControlledSocketMode::MALFORMED) {
		const std::string body = "{SECRET_MALFORMED";
		return "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" +
		       body;
	}
	if (response_mode == ControlledSocketMode::OVERSIZED_HEADER) {
		return "HTTP/1.1 200 OK\r\nX-Controlled: " + std::string(17000, 'h') +
		       "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	}
	if (response_mode == ControlledSocketMode::OVERSIZED_RESPONSE) {
		return "HTTP/1.1 200 OK\r\nContent-Length: 65537\r\nConnection: close\r\n\r\n" + std::string(65537, 'b');
	}
	if (response_mode == ControlledSocketMode::OVERSIZED_CHUNK_FRAMING) {
		return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n1;fixture=" +
		       std::string(65537, 'f') + "\r\n{\r\n0\r\n\r\n";
	}
	if (response_mode == ControlledSocketMode::MALFORMED_CHUNK_EXTENSION) {
		return "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n2;bad "
		       "name=x\r\n{}\r\n0\r\n\r\n";
	}
	if (response_mode == ControlledSocketMode::CHUNKED_SUCCESS) {
		const auto split = success_body.size() / 2;
		char first_size[32];
		char second_size[32];
		(void)snprintf(first_size, sizeof(first_size), "%zx", split);
		(void)snprintf(second_size, sizeof(second_size), "%zx", success_body.size() - split);
		return std::string("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n"
		                   "Connection: close\r\n\r\n") +
		       first_size + "; sig=abc; flag\r\n" + success_body.substr(0, split) + "\r\n" + second_size + "\r\n" +
		       success_body.substr(split) + "\r\n0\r\nX-Checksum: ok\r\n\r\n";
	}
	if (response_mode == ControlledSocketMode::GZIP_EXACT_DECOMPRESSED_LIMIT ||
	    response_mode == ControlledSocketMode::GZIP_OVER_DECOMPRESSED_LIMIT) {
		const auto body = response_mode == ControlledSocketMode::GZIP_EXACT_DECOMPRESSED_LIMIT
		                      ? Binary(GZIP_EXACT, sizeof(GZIP_EXACT))
		                      : Binary(GZIP_OVER, sizeof(GZIP_OVER));
		return "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Type: application/json\r\nContent-Length: " +
		       std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	}
	return "";
}

std::string ControlledSocketService::PaginatedResponse(uint64_t page_index) const {
	const std::string body = page_index == 1   ? "[{\"id\":1,\"full_name\":\"first\",\"private\":true,\"fork\":false,"
	                                             "\"archived\":false,\"visibility\":\"private\"}]"
	                         : page_index == 2 ? "[]"
	                                           : "[{\"id\":3,\"full_name\":\"third\",\"private\":true,\"fork\":false,"
	                                             "\"archived\":false,\"visibility\":\"private\"}]";
	const std::string link = page_index < 3 ? "Link: <https://api.github.com/user/repos?per_page=100&page=" +
	                                              std::to_string(page_index + 1) + "&visibility=private>; rel=next\r\n"
	                                        : "";
	return "HTTP/1.1 200 OK\r\n" + link +
	       "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) +
	       "\r\nConnection: close\r\n\r\n" + body;
}

void ControlledSocketService::Serve() noexcept {
	const uint64_t expected_connections = mode == ControlledSocketMode::BLOCK_THEN_AUTHENTICATED_SUCCESS ? 2
	                                      : mode == ControlledSocketMode::PAGINATED_REPOSITORIES         ? 3
	                                                                                                     : 1;
	for (uint64_t connection_index = 0; connection_index < expected_connections; connection_index++) {
		sockaddr_in peer;
		socklen_t peer_length = sizeof(peer);
		const auto accepted = accept(listener, reinterpret_cast<sockaddr *>(&peer), &peer_length);
		if (accepted < 0) {
			return;
		}
		client.store(accepted, std::memory_order_release);
		connection_count.fetch_add(1, std::memory_order_relaxed);
		std::string received;
		char buffer[2048];
		while (!RequestComplete(received) && received.size() < 65536) {
			const auto count = recv(accepted, buffer, sizeof(buffer), 0);
			if (count <= 0) {
				break;
			}
			try {
				received.append(buffer, static_cast<std::size_t>(count));
			} catch (...) {
				break;
			}
		}
		{
			std::lock_guard<std::mutex> guard(mutex);
			request = received;
			requests.push_back(received);
			request_ready = true;
		}
		condition.notify_all();
		if (mode == ControlledSocketMode::BLOCK) {
			std::unique_lock<std::mutex> guard(mutex);
			condition.wait(guard, [&]() { return stop; });
		} else if (mode == ControlledSocketMode::PARTIAL_RESPONSE_BLOCK) {
			(void)SendAll(accepted, "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 64\r\n"
			                        "Connection: close\r\n\r\n{\"partial\":");
			std::unique_lock<std::mutex> guard(mutex);
			condition.wait(guard, [&]() { return stop; });
		} else if (mode == ControlledSocketMode::BLOCK_THEN_AUTHENTICATED_SUCCESS && connection_index == 0) {
			while (recv(accepted, buffer, sizeof(buffer), 0) > 0) {
			}
		} else if (mode != ControlledSocketMode::DISCONNECT) {
			if (mode == ControlledSocketMode::PAGINATED_REPOSITORIES) {
				(void)SendAll(accepted, PaginatedResponse(connection_index + 1));
				(void)shutdown(accepted, SHUT_RDWR);
				close(accepted);
				client.store(-1, std::memory_order_release);
				continue;
			}
			const auto response_mode = mode == ControlledSocketMode::BLOCK_THEN_AUTHENTICATED_SUCCESS
			                               ? ControlledSocketMode::AUTHENTICATED_SUCCESS
			                               : mode;
			(void)SendAll(accepted, Response(response_mode));
		}
		(void)shutdown(accepted, SHUT_RDWR);
		close(accepted);
		client.store(-1, std::memory_order_release);
	}
}

} // namespace cuac_test
