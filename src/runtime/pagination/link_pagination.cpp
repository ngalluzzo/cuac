#include "cuac/internal/runtime/pagination/link_pagination.hpp"

#include "cuac/internal/runtime/pagination/link_header.hpp"

#include <limits>
#include <new>
#include <utility>

namespace cuac {
namespace internal {
namespace {

const char *const PAGINATION_FIELD = "pagination.next";

[[noreturn]] void ThrowMalformed() {
	throw LinkPaginationError(LinkPaginationErrorKind::MALFORMED, PAGINATION_FIELD,
	                          "Link pagination metadata is malformed");
}

[[noreturn]] void ThrowPolicy() {
	throw LinkPaginationError(LinkPaginationErrorKind::POLICY, PAGINATION_FIELD,
	                          "Link pagination target is outside the accepted policy");
}

[[noreturn]] void ThrowState() {
	throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD, "Link pagination state cannot advance");
}

uint64_t ParsePositiveDecimal(const std::string &value) {
	if (value.empty() || value[0] < '1' || value[0] > '9') {
		ThrowPolicy();
	}
	uint64_t result = 0;
	for (const auto character : value) {
		if (character < '0' || character > '9') {
			ThrowPolicy();
		}
		const auto digit = static_cast<uint64_t>(character - '0');
		if (result > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
			ThrowPolicy();
		}
		result = result * 10 + digit;
	}
	return result;
}

uint64_t ValidateNextTarget(const std::string &target, uint64_t current_page,
                            const AdmittedPaginatedRestRequestProfile &profile) {
	const std::string authority = profile.Scheme() + "://" + profile.Host();
	if (target.compare(0, authority.size(), authority) != 0) {
		ThrowPolicy();
	}
	std::size_t offset = authority.size();
	const auto explicit_port = ":" + std::to_string(profile.Port());
	if (target.compare(offset, explicit_port.size(), explicit_port) == 0) {
		offset += explicit_port.size();
	} else if (profile.Scheme() != "https" || profile.Port() != 443) {
		// RFC 3986's omitted-port form denotes the scheme default. V1 is HTTPS
		// only, so omission is equivalent to 443 and can never stand for a typed
		// nondefault operation port.
		ThrowPolicy();
	}
	const std::string path_and_query = profile.Path() + "?";
	if (target.compare(offset, path_and_query.size(), path_and_query) != 0) {
		ThrowPolicy();
	}
	offset += path_and_query.size();
	if (offset >= target.size() || target.find('#', offset) != std::string::npos) {
		ThrowPolicy();
	}

	std::vector<std::pair<std::string, std::string>> received;
	uint64_t parsed_page = 0;
	while (offset < target.size()) {
		const auto separator = target.find('&', offset);
		const auto field_end = separator == std::string::npos ? target.size() : separator;
		if (field_end == offset) {
			ThrowPolicy();
		}
		const auto equals = target.find('=', offset);
		if (equals == std::string::npos || equals >= field_end || equals == offset ||
		    target.find('=', equals + 1) < field_end) {
			ThrowPolicy();
		}
		const auto name = target.substr(offset, equals - offset);
		const auto value = target.substr(equals + 1, field_end - equals - 1);
		for (const auto &field : received) {
			if (field.first == name) {
				ThrowPolicy();
			}
		}
		if (name == profile.PageNumberParameter()) {
			parsed_page = ParsePositiveDecimal(value);
		}
		received.push_back({name, value});
		if (separator == std::string::npos) {
			break;
		}
		if (separator + 1 == target.size()) {
			ThrowPolicy();
		}
		offset = separator + 1;
	}
	if (current_page > std::numeric_limits<uint64_t>::max() - profile.PageIncrement()) {
		ThrowPolicy();
	}
	const auto next_page = current_page + profile.PageIncrement();
	// RFC 0017: the continuation URL need only match the exact origin, path,
	// and page-number progression. The full query-multiset comparison was
	// dropped because Runtime reconstructs the actual request locally from
	// the admitted profile's declared parameters — the continuation URL is a
	// verified signal, never a dereferenced fetch target. Non-page-number
	// query parameters in the URL are ignored.
	if (parsed_page != next_page) {
		ThrowPolicy();
	}
	return parsed_page;
}

class NextTargetSelector final : public LinkHeaderValueVisitor {
public:
	NextTargetSelector() : found_next(false) {
	}

	void Visit(const std::string &target, bool has_next_relation) override {
		if (!has_next_relation) {
			return;
		}
		if (found_next) {
			ThrowPolicy();
		}
		found_next = true;
		next_target = target;
	}

	bool FoundNext() const noexcept {
		return found_next;
	}

	const std::string &NextTarget() const noexcept {
		return next_target;
	}

private:
	bool found_next;
	std::string next_target;
};

LinkPageTransition ParseTransition(const std::vector<std::string> &fields, uint64_t current_page,
                                   const AdmittedPaginatedRestRequestProfile &profile) {
	NextTargetSelector selector;
	try {
		ParseLinkHeaderFields(fields, selector);
	} catch (const LinkHeaderSyntaxError &) {
		ThrowMalformed();
	}
	if (!selector.FoundNext()) {
		return {false, 0};
	}
	return {true, ValidateNextTarget(selector.NextTarget(), current_page, profile)};
}

} // namespace

LinkPaginationError::LinkPaginationError(LinkPaginationErrorKind kind_p, std::string field_p,
                                         std::string safe_message_p)
    : kind(kind_p), field(std::move(field_p)), safe_message(std::move(safe_message_p)) {
}

const char *LinkPaginationError::what() const noexcept {
	return safe_message.c_str();
}

LinkPaginationErrorKind LinkPaginationError::Kind() const noexcept {
	return kind;
}

const std::string &LinkPaginationError::Field() const noexcept {
	return field;
}

const std::string &LinkPaginationError::SafeMessage() const noexcept {
	return safe_message;
}

LinkPaginationState::LinkPaginationState(const AdmittedPaginatedRestRequestProfile &profile_p)
    : profile(profile_p), current_page(profile.FirstPage()), seen_page_count(1), exhausted(false), failed(false) {
}

LinkPageTransition LinkPaginationState::Advance(const std::vector<std::string> &link_field_values) {
	if (failed || exhausted) {
		ThrowState();
	}
	try {
		const auto transition = ParseTransition(link_field_values, current_page, profile);
		if (!transition.has_next) {
			exhausted = true;
			return transition;
		}
		seen_page_count++;
		current_page = transition.next_page;
		return transition;
	} catch (const LinkPaginationError &) {
		failed = true;
		throw;
	} catch (const std::bad_alloc &) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD,
		                          "Link pagination state exceeded available memory");
	} catch (...) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD, "Link pagination state failed");
	}
}

LinkPageTransition LinkPaginationState::AdvanceBody(const std::string &next_url) {
	if (failed || exhausted) {
		ThrowState();
	}
	try {
		if (next_url.empty()) {
			// JSON null, an absent path, or an empty string all mean "no next
			// page." This is the normal terminal signal for response_next.
			exhausted = true;
			return {false, 0};
		}
		// Apply the same reconstruct-and-verify rule a Link header target
		// would face. The body-extracted candidate must already be in
		// canonical ASCII percent-encoded form; no normalization is applied
		// (see docs/RUNTIME_CONTRACTS.md body-signaled REST pagination).
		const auto parsed_page = ValidateNextTarget(next_url, current_page, profile);
		seen_page_count++;
		current_page = parsed_page;
		return {true, parsed_page};
	} catch (const LinkPaginationError &) {
		failed = true;
		throw;
	} catch (const std::bad_alloc &) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD,
		                          "Link pagination state exceeded available memory");
	} catch (...) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD, "Link pagination state failed");
	}
}

LinkPageTransition LinkPaginationState::AdvanceByCount(std::size_t decoded_row_count) {
	if (failed || exhausted) {
		ThrowState();
	}
	try {
		// There is no server-supplied signal to validate: exhaustion is
		// inferred purely from the just-decoded page's row count against the
		// admitted profile's declared page size.
		if (decoded_row_count == 0 || decoded_row_count < profile.PageSize()) {
			exhausted = true;
			return {false, 0};
		}
		if (current_page > std::numeric_limits<uint64_t>::max() - profile.PageIncrement()) {
			ThrowPolicy();
		}
		const auto next_page = current_page + profile.PageIncrement();
		seen_page_count++;
		current_page = next_page;
		return {true, next_page};
	} catch (const LinkPaginationError &) {
		failed = true;
		throw;
	} catch (const std::bad_alloc &) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD,
		                          "Link pagination state exceeded available memory");
	} catch (...) {
		failed = true;
		throw LinkPaginationError(LinkPaginationErrorKind::STATE, PAGINATION_FIELD, "Link pagination state failed");
	}
}

uint64_t LinkPaginationState::CurrentPage() const noexcept {
	return current_page;
}

bool LinkPaginationState::Exhausted() const noexcept {
	return exhausted;
}

bool LinkPaginationState::Failed() const noexcept {
	return failed;
}

std::size_t LinkPaginationState::SeenPageCount() const noexcept {
	return seen_page_count;
}

} // namespace internal
} // namespace cuac
