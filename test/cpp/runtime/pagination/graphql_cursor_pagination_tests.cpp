#include "cuac/internal/runtime/pagination/graphql_cursor_pagination.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char *message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

void TestNullFirstCursorAndTerminalExhaustion() {
	cuac::internal::GraphqlCursorState state(32, 512);
	Require(state.CurrentCursor() == nullptr && state.RequestedPages() == 0, "first GraphQL cursor must be null");
	state.MarkRequestStarted();
	state.Advance(true, "cursor-1");
	Require(state.CurrentCursor() && *state.CurrentCursor() == "cursor-1" && state.RequestedPages() == 1,
	        "accepted continuation must become the next request cursor");
	state.MarkRequestStarted();
	state.Advance(false, "ignored-terminal-cursor");
	Require(state.IsExhausted() && state.CurrentCursor() == nullptr,
	        "hasNextPage false must exhaust without retaining a cursor");
}

void TestRepeatedCursorFailsTerminally() {
	cuac::internal::GraphqlCursorState state(32, 512);
	state.MarkRequestStarted();
	state.Advance(true, "same");
	state.MarkRequestStarted();
	try {
		state.Advance(true, "same");
		throw std::runtime_error("repeated cursor must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Field() == "pagination.cursor" && state.IsFailed() && state.CurrentCursor() == nullptr,
		        "repeated cursor failure must be redacted and terminal");
	}
}

void TestLastAuthorizedPageRetainsContinuationForCommonAccounting() {
	cuac::internal::GraphqlCursorState state(32, 512);
	for (uint64_t page = 1; page <= 31; page++) {
		state.MarkRequestStarted();
		state.Advance(true, "cursor-" + std::to_string(page));
	}
	state.MarkRequestStarted();
	state.Advance(true, "cursor-32");
	Require(!state.IsFailed() && state.RequestedPages() == 32 && state.CurrentCursor() &&
	            *state.CurrentCursor() == "cursor-32",
	        "last authorized response did not preserve its continuation for common accounting");
	try {
		state.MarkRequestStarted();
		throw std::runtime_error("cursor state accepted a 33rd request without common accounting");
	} catch (const cuac::internal::GraphqlCursorError &) {
		Require(state.IsFailed() && state.RequestedPages() == 32,
		        "cursor state fallback must still deny a request after its page authority");
	}
}

void TestPersistentCursorMemoryAndByteBoundary() {
	cuac::internal::GraphqlCursorState state(32, 512);
	const auto empty_state_bytes = state.RetainedMemoryBytes();
	state.MarkRequestStarted();
	state.Advance(true, std::string(512, 'x'));
	Require(state.CurrentCursor() && state.CurrentCursor()->size() == 512 &&
	            state.RetainedMemoryBytes() >= empty_state_bytes + 512,
	        "exact cursor boundary and persistent storage must be measurable");
	state.MarkRequestStarted();
	try {
		state.Advance(true, std::string(513, 'y'));
		throw std::runtime_error("oversized cursor must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Field() == "pagination.cursor" && state.IsFailed() && state.RetainedMemoryBytes() == 0,
		        "oversized cursor must release persistent state terminally");
	}
}

void TestCursorErrorClassification() {
	using cuac::internal::GraphqlCursorErrorKind;
	// Profile invalid -> PROFILE (-> FailureClass::CONFIGURATION).
	try {
		cuac::internal::GraphqlCursorState state(0, 512);
		(void)state;
		throw std::runtime_error("zero max_pages cursor profile must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Kind() == GraphqlCursorErrorKind::PROFILE, "invalid cursor profile was not PROFILE");
	}
	// Page authority exceeded -> RESOURCE_BUDGET.
	cuac::internal::GraphqlCursorState authority(1, 512);
	authority.MarkRequestStarted();
	authority.Advance(true, "cursor-1");
	try {
		authority.MarkRequestStarted();
		throw std::runtime_error("cursor state accepted a request beyond max_pages");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Kind() == GraphqlCursorErrorKind::RESOURCE_BUDGET,
		        "page-authority exhaustion was not RESOURCE_BUDGET");
	}
	// Missing continuation cursor -> PROTOCOL.
	cuac::internal::GraphqlCursorState missing(32, 512);
	missing.MarkRequestStarted();
	try {
		missing.Advance(true, "");
		throw std::runtime_error("missing cursor must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Kind() == GraphqlCursorErrorKind::PROTOCOL, "missing cursor was not PROTOCOL");
	}
	// Oversized cursor -> RESOURCE_BUDGET.
	cuac::internal::GraphqlCursorState oversized(32, 512);
	oversized.MarkRequestStarted();
	try {
		oversized.Advance(true, std::string(513, 'z'));
		throw std::runtime_error("oversized cursor must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Kind() == GraphqlCursorErrorKind::RESOURCE_BUDGET, "oversized cursor was not RESOURCE_BUDGET");
	}
	// Repeated cursor -> PROTOCOL.
	cuac::internal::GraphqlCursorState repeated(32, 512);
	repeated.MarkRequestStarted();
	repeated.Advance(true, "dup");
	repeated.MarkRequestStarted();
	try {
		repeated.Advance(true, "dup");
		throw std::runtime_error("repeated cursor must fail");
	} catch (const cuac::internal::GraphqlCursorError &error) {
		Require(error.Kind() == GraphqlCursorErrorKind::PROTOCOL, "repeated cursor was not PROTOCOL");
	}
}

} // namespace

int main() {
	try {
		TestNullFirstCursorAndTerminalExhaustion();
		TestRepeatedCursorFailsTerminally();
		TestLastAuthorizedPageRetainsContinuationForCommonAccounting();
		TestPersistentCursorMemoryAndByteBoundary();
		TestCursorErrorClassification();
		std::cout << "GraphQL cursor pagination tests passed\n";
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return EXIT_FAILURE;
	}
}
