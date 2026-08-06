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
//
// RFC 0029 generalized this mechanism from GraphQL-only to protocol-neutral.
// Nothing here is GraphQL- or REST-specific: it is a bounded forward traversal
// over opaque received tokens. GraphQL reaches it through the structured Relay
// profile; REST reaches it through the response_cursor strategy. There is
// exactly one such state machine on purpose — two would have to be kept
// identical by review alone, and would drift at the first divergent fix.
enum class OpaqueCursorErrorKind : uint8_t {
	// -> FailureClass::CONFIGURATION: invalid cursor profile.
	PROFILE,
	// -> FailureClass::RESOURCE_BUDGET: page authority, cursor byte budget, or memory.
	RESOURCE_BUDGET,
	// -> FailureClass::PROTOCOL: cursor state-machine or GraphQL shape violation.
	PROTOCOL
};

class OpaqueCursorError : public std::exception {
public:
	OpaqueCursorError(OpaqueCursorErrorKind kind, std::string field, std::string safe_message);
	const char *what() const noexcept override;
	const std::string &Field() const noexcept;
	const std::string &SafeMessage() const noexcept;
	OpaqueCursorErrorKind Kind() const noexcept;

private:
	OpaqueCursorErrorKind kind;
	std::string field;
	std::string safe_message;
};

// One stream owns one forward cursor state. The first request carries no cursor
// at all. Every accepted continuation is nonempty, unseen, and bounded;
// exhaustion and any rejected transition are terminal. No cursor grants
// ordering, snapshot, resume, or deduplication.
//
// The unseen-token set is what replaces the arithmetic progress proof a
// page-numbered strategy gets for free: an opaque token cannot be checked
// against a locally reconstructed expectation, so a repeat is the only
// detectable loop signal, backed by the hard page ceiling.
class OpaqueCursorState {
public:
	OpaqueCursorState(uint64_t max_pages, uint64_t max_cursor_bytes);
	OpaqueCursorState(const OpaqueCursorState &) = delete;

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
	[[noreturn]] void Reject(OpaqueCursorErrorKind kind, std::string field, std::string safe_message);

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
