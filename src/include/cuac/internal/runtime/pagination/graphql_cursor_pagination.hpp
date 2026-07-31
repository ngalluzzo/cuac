#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>

namespace cuac {
namespace internal {

// RFC 0021: structural discriminator for the distinct failure classes a cursor
// transition can produce, so the scan catch boundary maps to a FailureClass
// without parsing the safe-message text. Mirrors the LinkPaginationErrorKind
// precedent. The field/safe_message still carry the specific reason for the
// rendered diagnostic.
enum class GraphqlCursorErrorKind : uint8_t {
	// -> FailureClass::CONFIGURATION: invalid cursor profile.
	PROFILE,
	// -> FailureClass::RESOURCE_BUDGET: page authority, cursor byte budget, or memory.
	RESOURCE_BUDGET,
	// -> FailureClass::PROTOCOL: cursor state-machine or GraphQL shape violation.
	PROTOCOL
};

class GraphqlCursorError : public std::exception {
public:
	GraphqlCursorError(GraphqlCursorErrorKind kind, std::string field, std::string safe_message);
	const char *what() const noexcept override;
	const std::string &Field() const noexcept;
	const std::string &SafeMessage() const noexcept;
	GraphqlCursorErrorKind Kind() const noexcept;

private:
	GraphqlCursorErrorKind kind;
	std::string field;
	std::string safe_message;
};

// One stream owns one forward cursor state. The first request uses null. Every
// accepted continuation is nonempty, unseen, and bounded; exhaustion and any
// rejected transition are terminal. No cursor grants ordering or resume.
class GraphqlCursorState {
public:
	GraphqlCursorState(uint64_t max_pages, uint64_t max_cursor_bytes);
	GraphqlCursorState(const GraphqlCursorState &) = delete;

	const std::string *CurrentCursor() const noexcept;
	uint64_t RequestedPages() const noexcept;
	uint64_t RetainedMemoryBytes() const noexcept;
	bool IsExhausted() const noexcept;
	bool IsFailed() const noexcept;

	void MarkRequestStarted();
	void Advance(bool has_next, std::string end_cursor);
	void Fail() noexcept;
	void Release() noexcept;

private:
	[[noreturn]] void Reject(GraphqlCursorErrorKind kind, std::string field, std::string safe_message);

	uint64_t max_pages;
	uint64_t max_cursor_bytes;
	uint64_t requested_pages;
	bool exhausted;
	bool failed;
	std::array<std::string, 32> seen;
	std::size_t seen_count;
};

} // namespace internal
} // namespace cuac
