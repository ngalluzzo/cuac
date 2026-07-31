#include "cuac/internal/runtime/pagination/graphql_cursor_pagination.hpp"

#include <algorithm>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>

namespace cuac {
namespace internal {

GraphqlCursorError::GraphqlCursorError(GraphqlCursorErrorKind kind_p, std::string field_p, std::string safe_message_p)
    : kind(kind_p), field(std::move(field_p)), safe_message(std::move(safe_message_p)) {
}

const char *GraphqlCursorError::what() const noexcept {
	return safe_message.c_str();
}

const std::string &GraphqlCursorError::Field() const noexcept {
	return field;
}

const std::string &GraphqlCursorError::SafeMessage() const noexcept {
	return safe_message;
}

GraphqlCursorErrorKind GraphqlCursorError::Kind() const noexcept {
	return kind;
}

GraphqlCursorState::GraphqlCursorState(uint64_t max_pages_p, uint64_t max_cursor_bytes_p)
    : max_pages(max_pages_p), max_cursor_bytes(max_cursor_bytes_p), requested_pages(0), exhausted(false), failed(false),
      seen(), seen_count(0) {
	if (max_pages == 0 || max_pages > 32 || max_cursor_bytes == 0 || max_cursor_bytes > 512) {
		throw GraphqlCursorError(GraphqlCursorErrorKind::PROFILE, "pagination.cursor",
		                         "GraphQL cursor profile is invalid");
	}
}

const std::string *GraphqlCursorState::CurrentCursor() const noexcept {
	return exhausted || failed || seen_count == 0 ? nullptr : &seen[seen_count - 1];
}

uint64_t GraphqlCursorState::RequestedPages() const noexcept {
	return requested_pages;
}

uint64_t GraphqlCursorState::RetainedMemoryBytes() const noexcept {
	uint64_t result = 0;
	for (std::size_t index = 0; index < seen_count; index++) {
		const auto object_begin = reinterpret_cast<std::uintptr_t>(&seen[index]);
		const auto object_end = object_begin + sizeof(seen[index]);
		const auto data = reinterpret_cast<std::uintptr_t>(seen[index].data());
		if (data >= object_begin && data < object_end) {
			continue;
		}
		const auto allocation = static_cast<uint64_t>(seen[index].capacity()) + 1;
		if (allocation > std::numeric_limits<uint64_t>::max() - result) {
			return std::numeric_limits<uint64_t>::max();
		}
		result += allocation;
	}
	return result;
}

bool GraphqlCursorState::IsExhausted() const noexcept {
	return exhausted;
}

bool GraphqlCursorState::IsFailed() const noexcept {
	return failed;
}

[[noreturn]] void GraphqlCursorState::Reject(GraphqlCursorErrorKind kind, std::string field, std::string safe_message) {
	failed = true;
	Release();
	throw GraphqlCursorError(kind, std::move(field), std::move(safe_message));
}

void GraphqlCursorState::MarkRequestStarted() {
	if (failed || exhausted || requested_pages >= max_pages) {
		Reject(GraphqlCursorErrorKind::RESOURCE_BUDGET, "pagination.cursor",
		       "GraphQL cursor traversal exceeded its page authority");
	}
	requested_pages++;
}

void GraphqlCursorState::Advance(bool has_next, std::string end_cursor) {
	if (failed || exhausted || requested_pages == 0) {
		Reject(GraphqlCursorErrorKind::PROTOCOL, "pagination.cursor", "GraphQL cursor state cannot advance");
	}
	if (!has_next) {
		exhausted = true;
		Release();
		return;
	}
	if (end_cursor.empty()) {
		Reject(GraphqlCursorErrorKind::PROTOCOL, "pagination.cursor", "GraphQL continuation cursor is missing");
	}
	if (static_cast<uint64_t>(end_cursor.size()) > max_cursor_bytes) {
		Reject(GraphqlCursorErrorKind::RESOURCE_BUDGET, "pagination.cursor",
		       "GraphQL continuation cursor exceeded its byte budget");
	}
	for (std::size_t index = 0; index < seen_count; index++) {
		if (seen[index] == end_cursor) {
			Reject(GraphqlCursorErrorKind::PROTOCOL, "pagination.cursor", "GraphQL continuation cursor repeated");
		}
	}
	if (seen_count >= seen.size()) {
		Reject(GraphqlCursorErrorKind::RESOURCE_BUDGET, "pagination.cursor",
		       "GraphQL cursor traversal exceeded its page authority");
	}
	static_assert(std::is_nothrow_move_assignable<std::string>::value,
	              "cursor transfer must not allocate replacement storage");
	seen[seen_count++] = std::move(end_cursor);
}

void GraphqlCursorState::Fail() noexcept {
	failed = true;
	Release();
}

void GraphqlCursorState::Release() noexcept {
	for (std::size_t index = 0; index < seen_count; index++) {
		std::string().swap(seen[index]);
	}
	seen_count = 0;
}

} // namespace internal
} // namespace cuac
