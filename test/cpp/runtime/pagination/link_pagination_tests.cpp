#include "cuac/internal/runtime/pagination/link_pagination.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"
#include "semantics/support/repository_graphql_scan_plan_test_fixtures.hpp"
#include "semantics/support/scan_plan_test_fixtures.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using cuac::internal::AdmittedPaginatedRestRequestProfile;
using cuac::internal::LinkPageTransition;
using cuac::internal::LinkPaginationError;
using cuac::internal::LinkPaginationErrorKind;
using cuac::internal::LinkPaginationState;
using cuac_test::Require;

const std::string CANARY = "private-repository-canary";

AdmittedPaginatedRestRequestProfile AdmitPaginatedRestProfile(bool selective) {
	const cuac::internal::HttpExecutionProfile execution_profile {
	    cuac::PlannedUrlScheme::HTTPS,
	    "api.github.com",
	    443,
	    false,
	    false,
	    false,
	    cuac::MAX_EXECUTION_MILLISECONDS,
	    100,
	    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	    cuac::RETRY_MAX_DELAY_MILLISECONDS,
	    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(
	    selective ? cuac_test::BuildRepositoryGithubPackagePrivateRepositoriesPlan(CUAC_SOURCE_ROOT, "fixture_secret")
	              : cuac_test::BuildRepositoryGithubPackageRestPlan(CUAC_SOURCE_ROOT, "authenticated_repositories",
	                                                                "fixture_secret"),
	    execution_profile);
	Require(admitted != nullptr, "Link pagination fixture did not pass repository admission");
	return *admitted;
}

const AdmittedPaginatedRestRequestProfile &BaseProfile() {
	static const auto profile = AdmitPaginatedRestProfile(false);
	return profile;
}

const AdmittedPaginatedRestRequestProfile &SelectiveProfile() {
	static const auto profile = AdmitPaginatedRestProfile(true);
	return profile;
}

void RequireRejected(const std::vector<std::string> &fields, LinkPaginationErrorKind expected_kind,
                     const std::string &label) {
	LinkPaginationState state(BaseProfile());
	bool rejected = false;
	try {
		state.Advance(fields);
	} catch (const LinkPaginationError &error) {
		rejected = true;
		Require(error.Kind() == expected_kind, label + " used the wrong error kind");
		Require(error.Field() == "pagination.next", label + " used the wrong policy field");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        label + " produced an empty or unbounded diagnostic");
		Require(error.SafeMessage().find(CANARY) == std::string::npos, label + " exposed received Link data");
		Require(error.SafeMessage().find("api.github.com") == std::string::npos,
		        label + " exposed a received destination");
	}
	Require(rejected, label + " was accepted");
	Require(state.Failed(), label + " did not make pagination terminal");
	Require(state.CurrentPage() == 1 && state.SeenPageCount() == 1,
	        label + " changed typed state before validation completed");

	bool terminal = false;
	try {
		state.Advance({});
	} catch (const LinkPaginationError &error) {
		terminal = error.Kind() == LinkPaginationErrorKind::STATE;
	}
	Require(terminal, label + " permitted continuation after rejection");
}

void TestExhaustionWithoutNext() {
	LinkPaginationState absent(BaseProfile());
	const auto absent_transition = absent.Advance({});
	Require(!absent_transition.has_next && absent_transition.next_page == 0,
	        "an absent Link field did not exhaust the source");
	Require(absent.Exhausted() && !absent.Failed(), "clean exhaustion used a failure state");

	LinkPaginationState non_next(BaseProfile());
	const auto non_next_transition = non_next.Advance({"<https://example.test/page/1>; rel=last"});
	Require(!non_next_transition.has_next && non_next.Exhausted(),
	        "Link metadata without rel=next did not exhaust the source");

	LinkPaginationState empty_list(BaseProfile());
	const auto empty_list_transition = empty_list.Advance({" , , \t", ""});
	Require(!empty_list_transition.has_next && empty_list.Exhausted(),
	        "RFC list empty elements did not produce clean exhaustion");

	LinkPaginationState empty_target(BaseProfile());
	const auto empty_target_transition = empty_target.Advance({"<>; rel=last"});
	Require(!empty_target_transition.has_next && empty_target.Exhausted(),
	        "an empty relative URI-reference was rejected");

	LinkPaginationState ip_literal(BaseProfile());
	const auto ip_literal_transition = ip_literal.Advance({"<https://[2001:db8::1]/page>; rel=last"});
	Require(!ip_literal_transition.has_next && ip_literal.Exhausted(), "a valid IPv6 URI-reference was rejected");

	bool terminal = false;
	try {
		absent.Advance({});
	} catch (const LinkPaginationError &error) {
		terminal = error.Kind() == LinkPaginationErrorKind::STATE;
	}
	Require(terminal, "exhausted pagination advanced again");
}

void TestAcceptedTransitions() {
	LinkPaginationState case_insensitive(BaseProfile());
	const auto uppercase =
	    case_insensitive.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=NEXT"});
	Require(uppercase.has_next && uppercase.next_page == 2,
	        "case-insensitive registered next relation silently truncated pagination");

	LinkPaginationState extensions(BaseProfile());
	const auto extension_transition =
	    extensions.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; foo; "
	                        "rel=\"https://example.test/relations/archive next\""});
	Require(extension_transition.has_next && extension_transition.next_page == 2,
	        "valid valueless or URI relation extensions suppressed registered next");

	LinkPaginationState anchored(BaseProfile());
	const auto anchored_transition =
	    anchored.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next; "
	                      "anchor=\"https://credential-canary.invalid/context\""});
	Require(!anchored_transition.has_next && anchored.Exhausted(),
	        "an alternate anchor granted response-relative continuation authority");

	LinkPaginationState duplicate_rel_exhaustion(BaseProfile());
	const auto duplicate_rel_exhaustion_transition = duplicate_rel_exhaustion.Advance(
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=prev; REL=next"});
	Require(!duplicate_rel_exhaustion_transition.has_next && duplicate_rel_exhaustion.Exhausted(),
	        "a later duplicate rel parameter overrode the first non-next relation");

	LinkPaginationState duplicate_rel_next(BaseProfile());
	const auto duplicate_rel_next_transition =
	    duplicate_rel_next.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next; REL=prev"});
	Require(duplicate_rel_next_transition.has_next && duplicate_rel_next_transition.next_page == 2,
	        "a later duplicate rel parameter overrode the first next relation");

	LinkPaginationState bare_duplicate_rel_exhaustion(BaseProfile());
	const auto bare_duplicate_rel_exhaustion_transition = bare_duplicate_rel_exhaustion.Advance(
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=prev; REL"});
	Require(!bare_duplicate_rel_exhaustion_transition.has_next && bare_duplicate_rel_exhaustion.Exhausted(),
	        "a valueless later rel changed first-rel exhaustion");

	LinkPaginationState bare_duplicate_rel_next(BaseProfile());
	const auto bare_duplicate_rel_next_transition =
	    bare_duplicate_rel_next.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next; REL"});
	Require(bare_duplicate_rel_next_transition.has_next && bare_duplicate_rel_next_transition.next_page == 2,
	        "a valueless later rel suppressed the first next relation");

	LinkPaginationState leading_empty_elements(BaseProfile());
	const auto leading_empty_transition =
	    leading_empty_elements.Advance({", , <https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	Require(leading_empty_transition.has_next && leading_empty_transition.next_page == 2,
	        "leading empty list elements suppressed a valid next relation");

	LinkPaginationState trailing_empty_elements(BaseProfile());
	const auto trailing_empty_transition =
	    trailing_empty_elements.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next, ,"});
	Require(trailing_empty_transition.has_next && trailing_empty_transition.next_page == 2,
	        "trailing empty list elements rejected a valid next relation");

	LinkPaginationState middle_empty_elements(BaseProfile());
	const auto middle_empty_transition =
	    middle_empty_elements.Advance({"<https://example.test/previous>; rel=prev,, ,"
	                                   "<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	Require(middle_empty_transition.has_next && middle_empty_transition.next_page == 2,
	        "middle empty list elements suppressed a valid next relation");

	LinkPaginationState state(BaseProfile());
	const auto second = state.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	Require(second.has_next && second.next_page == 2, "the fixed page-two target was rejected");
	Require(state.CurrentPage() == 2 && state.SeenPageCount() == 2,
	        "accepted page two was not recorded as typed state");

	const auto third = state.Advance(
	    {"<https://example.test/previous>; rel=prev, "
	     "<https://api.github.com:443/user/repos?page=3&per_page=100>; title=\"a,b;c\"; REL=\"prev next\""});
	Require(third.has_next && third.next_page == 3, "combined Link grammar did not produce page three");
	Require(state.CurrentPage() == 3 && state.SeenPageCount() == 3,
	        "accepted page three was not recorded exactly once");

	const auto end = state.Advance({"<https://api.github.com/user/repos?page=3&per_page=100>; rel=last"});
	Require(!end.has_next && state.Exhausted(), "final non-next Link metadata did not end pagination");
}

void TestMalformedLinkGrammar() {
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"https://api.github.com/user/repos?per_page=100&page=2; rel=next", "missing target brackets"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2; rel=next", "unterminated target"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2> rel=next", "missing parameter separator"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel", "missing parameter value"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>", "missing required relation parameter"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next", "unterminated quote"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\" next\"", "leading relation space"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next \"", "trailing relation space"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\tprev\"", "relation tab"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"/not-a-relation\"",
	     "relative URI relation type"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"https://[::1\"",
	     "malformed extension relation URI"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2>; rel=\"next\\", "unterminated escape"},
	    {"<https://api.github.com/user/repos?per_page=100&page=2 bad>; rel=next", "invalid URI character"},
	    {"<https://api.github.com/user/repos?per_page=100&page=%ZZ>; rel=next", "malformed percent escape"},
	    {"<https://[2001:db8::1/page>; rel=last", "unmatched IP-literal bracket"},
	    {"<https://[not-ip]/page>; rel=last", "malformed IP literal"},
	    {"<https://example.test:not-a-port/page>; rel=last", "malformed authority port"},
	    {"<https://first@second@example.test/page>; rel=last", "multiple user-info delimiters"},
	    {"<https://example.test/page>; rel=last; anchor=\"https://[::1\"", "malformed anchor URI"}};
	for (const auto &test_case : cases) {
		RequireRejected({test_case.first}, LinkPaginationErrorKind::MALFORMED, test_case.second);
	}
	RequireRejected({std::string(129, ',')}, LinkPaginationErrorKind::MALFORMED,
	                "unreasonable empty list element count");
}

void TestMultipleNextTargets() {
	RequireRejected({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next, "
	                 "<https://api.github.com:443/user/repos?page=2&per_page=100>; rel=next"},
	                LinkPaginationErrorKind::POLICY, "two next targets in one field");
	RequireRejected({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next",
	                 "<https://api.github.com/user/repos?page=2&per_page=100>; rel=next"},
	                LinkPaginationErrorKind::POLICY, "two next targets in physical fields");
}

void TestDeniedNextTargets() {
	// RFC 0017 relaxed ValidateNextTarget: only origin, path, and page-number
	// are checked. Non-page-number query parameters (page_size values, extra
	// fields, encoded names) are no longer cause for rejection because Runtime
	// reconstructs the actual request from its own declared parameters — the
	// continuation URL is a verified signal, never a fetch target.
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"https://evil.example.test/user/repos?per_page=100&page=2", "wrong origin"},
	    {"https://api.github.com:444/user/repos?per_page=100&page=2", "wrong explicit port"},
	    {"https://user@api.github.com/user/repos?per_page=100&page=2", "user information"},
	    {"https://api.github.com.evil.test/user/repos?per_page=100&page=2", "authority suffix"},
	    {"https://api.github.com/users/repos?per_page=100&page=2", "wrong path"},
	    {"https://api.github.com/user/repos/?per_page=100&page=2", "trailing path slash"},
	    {"https://api.github.com/user/repos?per_page=100&page=2#fragment", "fragment"},
	    {"https://api.github.com/user/%72epos?per_page=100&page=2", "encoded path"},
	    {"https://api.github.com/user/repos?per_page=100&page=%32", "encoded page number"},
	    {"https://api.github.com/user/repos?per_page=100", "missing page field"},
	    {"https://api.github.com/user/repos?per_page=100&per_page=100&page=2", "duplicate per-page field"},
	    {"https://api.github.com/user/repos?per_page=100&page=2&page=2", "duplicate page field"},
	    {"https://api.github.com/user/repos?per_page=100&page=2&", "trailing empty field"},
	    {"https://api.github.com/user/repos?&per_page=100&page=2", "leading empty field"},
	    {"https://api.github.com/user/repos?per_page=100&&page=2", "middle empty field"},
	    {"https://api.github.com/user/repos?per_page=100&page=", "empty page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=0", "zero page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=02", "leading-zero page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=+2", "signed page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=3", "nonincrementing page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=18446744073709551616", "overflowing page number"},
	    {"https://api.github.com/user/repos?per_page=100=100&page=2", "extra equals sign"}};
	for (const auto &test_case : cases) {
		RequireRejected({"<" + test_case.first + ">; rel=next; title=\"" + CANARY + "\""},
		                LinkPaginationErrorKind::POLICY, test_case.second);
	}
}

void TestRepeatedTypedPageIsRejected() {
	LinkPaginationState state(BaseProfile());
	state.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	bool rejected = false;
	try {
		state.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	} catch (const LinkPaginationError &error) {
		rejected = error.Kind() == LinkPaginationErrorKind::POLICY && error.Field() == "pagination.next";
	}
	Require(rejected && state.Failed(), "a repeated typed page was not a terminal policy error");
	Require(state.CurrentPage() == 2 && state.SeenPageCount() == 2,
	        "repeated pagination metadata mutated accepted state");
}

void TestSelectiveTargetFieldSet() {
	// RFC 0017: the selective (visibility=private) target no longer requires
	// the continuation URL to echo the visibility parameter. Runtime
	// reconstructs the request with visibility=private locally; the
	// continuation URL only needs the right origin, path, and page number.
	LinkPaginationState accepted(SelectiveProfile());
	const auto transition =
	    accepted.Advance({"<https://api.github.com/user/repos?visibility=private&page=2&per_page=100>; rel=next"});
	Require(transition.has_next && transition.next_page == 2,
	        "selective Link did not advance with the full admitted field set");

	// A continuation URL that omits visibility is now accepted.
	LinkPaginationState without_visibility(SelectiveProfile());
	const auto without_transition =
	    without_visibility.Advance({"<https://api.github.com/user/repos?per_page=100&page=2>; rel=next"});
	Require(without_transition.has_next && without_transition.next_page == 2,
	        "selective Link rejected a URL that omits visibility (RFC 0017 relaxation)");

	// Duplicate query names are still rejected by the parser.
	LinkPaginationState duplicate(SelectiveProfile());
	bool duplicate_rejected = false;
	try {
		duplicate.Advance(
		    {"<https://api.github.com/user/repos?per_page=100&page=2&visibility=private&visibility=private>; "
		     "rel=next"});
	} catch (const LinkPaginationError &error) {
		duplicate_rejected = error.Kind() == LinkPaginationErrorKind::POLICY;
	}
	Require(duplicate_rejected && duplicate.Failed(), "duplicate visibility field was accepted");
}

// --- response_next (body-sourced continuation) tests ---
//
// AdvanceBody applies the same ValidateNextTarget reconstruct-and-verify rule
// as Advance, but reads the candidate URL from a decoded JSON body path rather
// than parsing it from Link header field values. These tests prove the two
// entry points produce identical transitions and rejections for the same
// underlying target URL, and that the body path handles its unique failure
// mode (empty/absent continuation = clean exhaustion).

const std::string EXPECTED_NEXT_URL = "https://api.github.com/user/repos?per_page=100&page=2";
const std::string EXPECTED_NEXT_URL_PAGE_3 = "https://api.github.com/user/repos?per_page=100&page=3";

void RequireBodyRejected(const std::string &body_url, LinkPaginationErrorKind expected_kind, const std::string &label) {
	LinkPaginationState state(BaseProfile());
	bool rejected = false;
	try {
		state.AdvanceBody(body_url);
	} catch (const LinkPaginationError &error) {
		rejected = true;
		Require(error.Kind() == expected_kind, label + " used the wrong error kind");
		Require(error.Field() == "pagination.next", label + " used the wrong policy field");
		Require(!error.SafeMessage().empty() && error.SafeMessage().size() <= 128,
		        label + " produced an empty or unbounded diagnostic");
		Require(error.SafeMessage().find(CANARY) == std::string::npos, label + " exposed received body data");
		Require(error.SafeMessage().find("api.github.com") == std::string::npos,
		        label + " exposed a received destination");
	}
	Require(rejected, label + " was accepted");
	Require(state.Failed(), label + " did not make pagination terminal");
}

void TestBodyExhaustionEmpty() {
	// An empty body URL is the normal terminal signal for response_next (the
	// JSON path was absent or the value was null). It must produce clean
	// exhaustion, not a failure.
	LinkPaginationState state(BaseProfile());
	const auto transition = state.AdvanceBody("");
	Require(!transition.has_next && transition.next_page == 0, "an empty body continuation did not exhaust the source");
	Require(state.Exhausted() && !state.Failed(), "clean body exhaustion used a failure state");
}

void TestBodyAcceptedTransition() {
	LinkPaginationState state(BaseProfile());
	const auto transition = state.AdvanceBody(EXPECTED_NEXT_URL);
	Require(transition.has_next && transition.next_page == 2,
	        "a valid body continuation did not advance to the expected page");
	Require(state.CurrentPage() == 2 && state.SeenPageCount() == 2 && !state.Exhausted() && !state.Failed(),
	        "body transition left inconsistent state");

	// Multi-page progression: page 2 → page 3
	const auto next_transition = state.AdvanceBody(EXPECTED_NEXT_URL_PAGE_3);
	Require(next_transition.has_next && next_transition.next_page == 3,
	        "a second body continuation did not advance to the expected page");
	Require(state.CurrentPage() == 3 && state.SeenPageCount() == 3, "second body transition left inconsistent state");
}

void TestBodyDeniedTargets() {
	// RFC 0017: only origin, path, and page-number are checked.
	const std::vector<std::pair<std::string, std::string>> cases = {
	    {"https://evil.example.test/user/repos?per_page=100&page=2", "wrong origin"},
	    {"https://api.github.com/user/repos?per_page=100", "missing page query"},
	    {"https://api.github.com/user/repos?per_page=100&page=1", "non-incrementing page number"},
	    {"https://api.github.com/user/repos?per_page=100&page=3", "skipped page number"},
	    {"https://api.github.com/other/path?per_page=100&page=2", "wrong path"},
	};
	for (const auto &test_case : cases) {
		RequireBodyRejected(test_case.first, LinkPaginationErrorKind::POLICY, test_case.second);
	}
}

void TestBodyRepeatedTypedPageIsRejected() {
	LinkPaginationState state(BaseProfile());
	state.AdvanceBody(EXPECTED_NEXT_URL);
	bool rejected = false;
	try {
		state.AdvanceBody(EXPECTED_NEXT_URL);
	} catch (const LinkPaginationError &error) {
		rejected = error.Kind() == LinkPaginationErrorKind::POLICY && error.Field() == "pagination.next";
	}
	Require(rejected && state.Failed(), "a repeated body continuation was not a terminal policy error");
	Require(state.CurrentPage() == 2 && state.SeenPageCount() == 2,
	        "repeated body continuation mutated accepted state");
}

void TestBodyDifferentialParity() {
	// The differential test RFC 0016 requires: for the same underlying page
	// sequence, header-sourced and body-sourced targets produce identical
	// transitions. Advance parses the URL from a Link header value;
	// AdvanceBody receives the URL directly. Both feed into the same
	// ValidateNextTarget. This test proves the parity holds for accepted
	// transitions, clean exhaustion, and every policy rejection.
	const std::vector<std::pair<std::string, std::string>> targets = {
	    {EXPECTED_NEXT_URL, "valid next-page target"},
	    {"https://api.github.com/user/repos?per_page=100&page=3", "skip-page target"},
	    {"https://evil.example.test/user/repos?per_page=100&page=2", "wrong-origin target"},
	    {"https://api.github.com/other/path?per_page=100&page=2", "wrong-path target"},
	    {"https://api.github.com/user/repos?page=2", "page-only target (no page_size)"},
	};
	for (const auto &target : targets) {
		LinkPaginationState header_state(BaseProfile());
		LinkPaginationState body_state(BaseProfile());
		bool header_rejected = false;
		bool body_rejected = false;
		LinkPageTransition header_transition {false, 0};
		LinkPageTransition body_transition {false, 0};
		try {
			header_transition = header_state.Advance({"<" + target.first + ">; rel=next"});
		} catch (const LinkPaginationError &) {
			header_rejected = true;
		}
		try {
			body_transition = body_state.AdvanceBody(target.first);
		} catch (const LinkPaginationError &) {
			body_rejected = true;
		}
		Require(header_rejected == body_rejected, target.second + ": header and body paths disagreed on acceptance");
		Require(header_transition.has_next == body_transition.has_next &&
		            header_transition.next_page == body_transition.next_page,
		        target.second + ": header and body paths produced different transitions");
		Require(header_state.Failed() == body_state.Failed(),
		        target.second + ": header and body paths left different failure states");
		Require(header_state.Exhausted() == body_state.Exhausted(),
		        target.second + ": header and body paths left different exhaustion states");
	}
}

AdmittedPaginatedRestRequestProfile ShortPageProfile() {
	const cuac::internal::HttpExecutionProfile execution_profile {
	    cuac::PlannedUrlScheme::HTTPS,
	    "api.github.com",
	    443,
	    false,
	    false,
	    false,
	    cuac::MAX_EXECUTION_MILLISECONDS,
	    100,
	    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	    cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	    cuac::RETRY_MAX_DELAY_MILLISECONDS,
	    cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
	auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(
	    cuac_test::BuildValidShortPagePlanFixture("fixture_secret"), execution_profile);
	Require(admitted != nullptr, "short_page fixture did not pass admission");
	return *admitted;
}

// RFC 0019: AdvanceByCount has no external signal to validate, so its unit
// tests directly exercise the count-comparison state machine rather than
// header/body grammar. The fixture's declared page_size is 3.
void TestCountAdvanceTerminatesOnShortPage() {
	LinkPaginationState state(ShortPageProfile());
	const auto transition = state.AdvanceByCount(2);
	Require(!transition.has_next && transition.next_page == 0,
	        "a page shorter than page_size did not exhaust short_page pagination");
	Require(state.Exhausted() && !state.Failed(), "clean short-page exhaustion used a failure state");
}

void TestCountAdvanceTerminatesOnEmptyPage() {
	LinkPaginationState state(ShortPageProfile());
	const auto transition = state.AdvanceByCount(0);
	Require(!transition.has_next && transition.next_page == 0, "an empty page did not exhaust short_page pagination");
	Require(state.Exhausted() && !state.Failed(), "clean empty-page exhaustion used a failure state");
}

void TestCountAdvanceAcceptsFullPagesAndAdvances() {
	LinkPaginationState state(ShortPageProfile());
	const auto first = state.AdvanceByCount(3);
	Require(first.has_next && first.next_page == 2, "a full first page did not advance short_page pagination");
	Require(state.CurrentPage() == 2 && state.SeenPageCount() == 2 && !state.Exhausted() && !state.Failed(),
	        "short_page transition left inconsistent state");
	const auto second = state.AdvanceByCount(3);
	Require(second.has_next && second.next_page == 3, "a second full page did not advance short_page pagination");
	Require(state.CurrentPage() == 3 && state.SeenPageCount() == 3, "second short_page transition lost state");
	const auto third = state.AdvanceByCount(1);
	Require(!third.has_next && state.Exhausted(), "a short page after two full pages did not exhaust the source");
}

void TestCountAdvanceRejectsContinuationAfterExhaustion() {
	LinkPaginationState state(ShortPageProfile());
	state.AdvanceByCount(0);
	bool terminal = false;
	try {
		state.AdvanceByCount(3);
	} catch (const LinkPaginationError &error) {
		terminal = error.Kind() == LinkPaginationErrorKind::STATE;
	}
	Require(terminal, "exhausted short_page pagination advanced again");
}

void TestBodyEncodingDivergenceRejected() {
	// RFC 0016 identified a genuinely new correctness edge case for
	// response_next: JSON \uXXXX unescaping can produce a body URL with
	// non-ASCII bytes where a Link header would have percent-encoded ASCII.
	// The scoping-spike decision is strict byte comparison: no normalization
	// is applied, and a non-canonical body URL fails closed at the POLICY
	// phase. The body URL contains a UTF-8 é (0xC3 0xA9) where the
	// reconstructed expectation has the ASCII path /user/repos. ValidateNextTarget
	// compares byte-for-byte and rejects.
	const std::string non_ascii_host = "https://api.github.com\xc3\xa9/user/repos?per_page=100&page=2";
	RequireBodyRejected(non_ascii_host, LinkPaginationErrorKind::POLICY, "non-ASCII body URL (encoding divergence)");
}

} // namespace

int main() {
	try {
		TestExhaustionWithoutNext();
		TestAcceptedTransitions();
		TestMalformedLinkGrammar();
		TestMultipleNextTargets();
		TestDeniedNextTargets();
		TestRepeatedTypedPageIsRejected();
		TestSelectiveTargetFieldSet();
		TestBodyExhaustionEmpty();
		TestBodyAcceptedTransition();
		TestBodyDeniedTargets();
		TestBodyRepeatedTypedPageIsRejected();
		TestBodyDifferentialParity();
		TestBodyEncodingDivergenceRejected();
		TestCountAdvanceTerminatesOnShortPage();
		TestCountAdvanceTerminatesOnEmptyPage();
		TestCountAdvanceAcceptsFullPagesAndAdvances();
		TestCountAdvanceRejectsContinuationAfterExhaustion();
		std::cout << "Link pagination tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Link pagination tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
