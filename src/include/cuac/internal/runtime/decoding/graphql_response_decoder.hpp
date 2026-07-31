#pragma once

#include "cuac/runtime/execution.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

struct GraphqlDecodeLimits {
	uint64_t max_records;
	uint64_t max_string_bytes;
	uint64_t max_json_nesting;
	uint64_t max_decoded_memory_bytes;
	std::chrono::steady_clock::time_point deadline;
};

struct DecodedGraphqlPage {
	std::vector<TypedRow> rows;
	bool has_next;
	std::string end_cursor;
	// Retained row storage after the cursor is transferred to pagination state.
	uint64_t retained_memory_bytes;
	// Temporary decoded cursor storage before that ownership transfer.
	uint64_t cursor_memory_bytes;
	// Maximum decoded-memory storage observed while producing the page. This
	// includes transient row slots while they are co-live with retained rows.
	uint64_t peak_memory_bytes;
};

// Strictly validates and decodes one complete GraphQL response envelope. A
// nonempty top-level errors array always becomes REMOTE_PROTOCOL before data
// can be published, without inspecting or copying remote error contents.
DecodedGraphqlPage DecodeGraphqlResponse(const std::string &body, const AdmittedGraphqlRequestProfile &profile,
                                         const GraphqlDecodeLimits &limits, ExecutionControl &control);

} // namespace internal
} // namespace cuac
