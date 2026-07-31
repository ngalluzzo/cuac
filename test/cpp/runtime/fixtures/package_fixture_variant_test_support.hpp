#pragma once

#include "package_fixture_execution.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cuac_test {
namespace variant_test {

class ManualControl final : public cuac::ExecutionControl {
public:
	ManualControl() : cancelled(false) {
	}

	bool IsCancellationRequested() const noexcept override {
		return cancelled.load(std::memory_order_acquire);
	}

	void Cancel() noexcept {
		cancelled.store(true, std::memory_order_release);
	}

private:
	std::atomic<bool> cancelled;
};

inline RuntimeFixtureResponsePage Response(std::string body, std::vector<RuntimeFixtureResponseHeader> headers = {}) {
	return {200, std::move(headers), std::move(body)};
}

inline std::string SearchPage() {
	return "{\"items\":[{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false}]}";
}

inline std::string GenericRestPage() {
	return "{\"records\":[{\"record_id\":1,\"record_label\":\"one\"}]}";
}

inline std::string DoubleColumnPage() {
	return "{\"items\":[{\"score\":1.5}]}";
}

inline std::string AuthenticatedRootObject() {
	return "{\"id\":11,\"login\":\"duckdb\",\"site_admin\":false}";
}

inline std::string GraphqlNode() {
	return "{\"id\":\"R1\",\"nameWithOwner\":\"duckdb/R1\",\"owner\":{\"login\":\"duckdb\"},"
	       "\"stargazerCount\":42,\"primaryLanguage\":null,\"isPrivate\":false,\"isArchived\":false,"
	       "\"updatedAt\":\"2026-07-01T00:00:00Z\"}";
}

inline std::string GraphqlPage() {
	return "{\"data\":{\"viewer\":{\"repositories\":{\"nodes\":[" + GraphqlNode() +
	       "],\"pageInfo\":{\"hasNextPage\":false,\"endCursor\":null}}}},\"errors\":[]}";
}

inline std::string NonGithubGraphqlPage() {
	return "{\"data\":{\"organization\":{\"eventFeed\":{\"nodes\":[{\"id\":\"event-g1\",\"active\":true,"
	       "\"stats\":{\"attendance\":120},\"classification\":{\"label\":\"public\"}}],\"pagination\":{"
	       "\"more\":false,\"next\":null}}}},\"errors\":[]}";
}

inline RuntimeFixtureTranscript AnonymousTranscript() {
	return {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(SearchPage())}};
}

inline RuntimeFixtureTranscript DoubleColumnTranscript() {
	return {RuntimeFixtureAuthorizationState::ANONYMOUS, {Response(DoubleColumnPage())}};
}

inline RuntimeFixtureTranscript GenericRestTranscript() {
	return {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response(GenericRestPage())}};
}

inline RuntimeFixtureTranscript GraphqlTranscript() {
	return {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response(GraphqlPage())}};
}

inline RuntimeFixtureTranscript NonGithubGraphqlTranscript() {
	return {RuntimeFixtureAuthorizationState::BEARER_PRESENT, {Response(NonGithubGraphqlPage())}};
}

} // namespace variant_test
} // namespace cuac_test
