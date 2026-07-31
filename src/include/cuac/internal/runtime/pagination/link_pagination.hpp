#pragma once

#include "cuac/internal/runtime/admission/http_plan_admission.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

// Private, DuckDB-free failure returned by the Link pagination service.
// Field and SafeMessage are bounded diagnostics chosen by Runtime; received
// Link values and target strings are never retained by or exposed through it.
enum class LinkPaginationErrorKind : uint8_t { MALFORMED, POLICY, STATE };

class LinkPaginationError : public std::exception {
public:
	LinkPaginationError(LinkPaginationErrorKind kind, std::string field, std::string safe_message);

	const char *what() const noexcept override;
	LinkPaginationErrorKind Kind() const noexcept;
	const std::string &Field() const noexcept;
	const std::string &SafeMessage() const noexcept;

private:
	LinkPaginationErrorKind kind;
	std::string field;
	std::string safe_message;
};

// The parser returns only a typed page transition. It deliberately returns no
// received URL, authority, path, query spelling, or request object. Executor
// integration reconstructs a request from its separately validated immutable
// operation and this typed page number.
struct LinkPageTransition {
	bool has_next;
	uint64_t next_page;
};

// Sequential state for an admitted paginated REST request profile. Resume and
// caller-selected starting state are unsupported. Advance parses physical Link
// field-values in receipt order, accepts zero or one rel=next target, and
// requires the profile's exact origin, path, page progression, and copied
// encoded query multiset. Omission, change, duplication, or extra query
// fields fail closed. AdvanceBody accepts a single body-extracted candidate
// URL (or nullopt for "no next page") and applies the same reconstruct-and-
// verify rule. Any error makes the state terminal so a caller cannot
// continue after rejected remote metadata. This object owns a copy of the
// immutable admitted profile, is scan-owned, and is not thread-safe; the owning
// stream supplies synchronization and resource/cancellation authority.
class LinkPaginationState {
public:
	explicit LinkPaginationState(const AdmittedPaginatedRestRequestProfile &profile);
	LinkPaginationState(const LinkPaginationState &) = default;

	LinkPageTransition Advance(const std::vector<std::string> &link_field_values);
	// Body-sourced continuation: an empty next_url means "no next page"
	// (the path was absent, the JSON value was null, or pagination is
	// exhausted). A non-empty URL is validated against the same
	// reconstruct-and-verify rule as a Link header target.
	LinkPageTransition AdvanceBody(const std::string &next_url);
	// RFC 0019: count-terminated continuation for short_page. There is no
	// external signal to validate — exhaustion is inferred purely from
	// comparing decoded_row_count against the admitted profile's declared
	// page size. Reuses this class's existing sequence bookkeeping; no
	// parallel state object is needed since no field here stores a
	// validated target string.
	LinkPageTransition AdvanceByCount(std::size_t decoded_row_count);

	uint64_t CurrentPage() const noexcept;
	bool Exhausted() const noexcept;
	bool Failed() const noexcept;
	std::size_t SeenPageCount() const noexcept;

private:
	const AdmittedPaginatedRestRequestProfile &profile;
	uint64_t current_page;
	std::size_t seen_page_count;
	bool exhausted;
	bool failed;
};

} // namespace internal
} // namespace cuac
