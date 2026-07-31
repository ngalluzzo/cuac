#pragma once

#include "cuac/internal/runtime/transport/curl_transfer.hpp"

#include <curl/curl.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

// Call-scoped response and callback state for one fresh curl easy handle. Curl
// callbacks are noexcept and communicate only through these bounded counters,
// retained buffers, and terminal flags. The transfer orchestrator owns error
// classification after curl_easy_perform returns; received bytes never enter a
// diagnostic. No field outlives PerformCurlTransfer.
struct CurlTransferState {
	enum class RetainedHeaderKind : uint8_t { NONE, RATE_LIMIT, DATE, LINK, RATE_LIMIT_LINK };

	CurlTransferState(ExecutionControl &control, const HttpLimits &limits, const CurlTransferProfile &profile);

	bool ShouldContinue() noexcept;

	ExecutionControl &control;
	const HttpLimits &limits;
	const CurlTransferProfile &profile;
	uint64_t header_bytes;
	uint64_t response_bytes;
	std::string body;
	std::vector<std::string> link_field_values;
	std::vector<HttpObservedHeader> rate_limit_fields;
	std::vector<std::string> date_field_values;
	bool cancelled;
	bool timed_out;
	bool header_oversized;
	bool response_oversized;
	bool decompressed_oversized;
	bool metadata_oversized;
	bool body_allocation_failed;
	bool metadata_allocation_failed;
	bool address_denied;
	bool socket_attempted;
	uint64_t metadata_bytes;
	bool header_section_complete;
	bool retry_after_present;
	bool transfer_encoding_seen;
	bool transfer_chunked;
	bool transfer_encoding_unsupported;
	bool content_encoded;
	RetainedHeaderKind retained_header_kind;
	std::size_t retained_header_index;
	std::size_t retained_link_index;
	CURL *easy_handle;
};

void ReleaseCurlLinkMetadata(CurlTransferState &state) noexcept;
std::size_t WriteCurlBody(char *data, std::size_t size, std::size_t count, void *opaque) noexcept;
std::size_t ReadCurlHeader(char *data, std::size_t size, std::size_t count, void *opaque) noexcept;
int CountIncomingCurlProtocolData(CURL *handle, curl_infotype type, char *data, std::size_t length,
                                  void *opaque) noexcept;
int ObserveCurlTransferProgress(void *opaque, curl_off_t total, curl_off_t current, curl_off_t upload_total,
                                curl_off_t upload_current) noexcept;
curl_socket_t OpenCurlPolicySocket(void *opaque, curlsocktype purpose, struct curl_sockaddr *address) noexcept;

} // namespace internal
} // namespace cuac
