#include "connector/support/package_compiler_test_fixtures.hpp"
#include "cuac/semantics/cache_policy.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "cuac/query/scan_request.hpp"
#include "query/support/live_scan_request.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <type_traits>

namespace {

using cuac_test::Require;

template <typename Exception, typename F>
void RequireThrows(F &&f, const std::string &message) {
	try {
		f();
		throw std::runtime_error(message);
	} catch (const Exception &) {
	} catch (const std::runtime_error &) {
		throw;
	} catch (...) {
		throw std::runtime_error(message + " (wrong exception type)");
	}
}

void TestFreshnessPolicyDefaults() {
	cuac::FreshnessPolicy off;
	Require(off.Mode() == cuac::CacheMode::OFF && !off.IsEnabled() && off.IsValid(),
	        "default freshness policy was not a valid OFF");
	Require(off.FreshMilliseconds() == 0 && off.StaleMilliseconds() == 0, "default policy had nonzero windows");
	Require(off == cuac::FreshnessPolicy::Off(), "default policy did not equal Off factory");

	Require(cuac::FreshnessPolicy::Off().Snapshot() == "cache_mode=off;fresh_ms=0;stale_ms=0", "OFF snapshot drifted");
}

void TestFreshnessPolicyFactoriesAndBounds() {
	const auto fresh = cuac::FreshnessPolicy::Fresh(60000);
	Require(fresh.Mode() == cuac::CacheMode::FRESH && fresh.IsEnabled() && fresh.IsValid() &&
	            fresh.FreshMilliseconds() == 60000,
	        "FRESH factory did not produce a valid enabled policy");
	Require(fresh.Snapshot() == "cache_mode=fresh;fresh_ms=60000;stale_ms=0", "FRESH snapshot drifted");

	const auto stale = cuac::FreshnessPolicy::StaleIfError(30000, 120000);
	Require(stale.Mode() == cuac::CacheMode::STALE_IF_ERROR && stale.IsEnabled() && stale.IsValid() &&
	            stale.FreshMilliseconds() == 30000 && stale.StaleMilliseconds() == 120000,
	        "STALE_IF_ERROR factory did not produce a valid policy");
	Require(stale.Snapshot() == "cache_mode=stale_if_error;fresh_ms=30000;stale_ms=120000",
	        "STALE_IF_ERROR snapshot drifted");

	Require(fresh != stale && fresh != cuac::FreshnessPolicy::Off(), "distinct policies compared equal");

	const auto max_fresh = cuac::FreshnessPolicy::Fresh(cuac::FreshnessPolicy::MAX_FRESH_MILLISECONDS);
	Require(max_fresh.IsValid(), "maximum fresh window was rejected");

	const auto max_stale = cuac::FreshnessPolicy::StaleIfError(1, cuac::FreshnessPolicy::MAX_STALE_MILLISECONDS);
	Require(max_stale.IsValid(), "maximum stale window was rejected");
}

void TestFreshnessPolicyRejection() {
	using cuac::FreshnessPolicy;
	RequireThrows<std::invalid_argument>([]() { (void)FreshnessPolicy::Fresh(0); },
	                                     "FRESH with zero window was accepted");
	RequireThrows<std::invalid_argument>(
	    []() { (void)FreshnessPolicy::Fresh(FreshnessPolicy::MAX_FRESH_MILLISECONDS + 1); },
	    "FRESH over hard max was accepted");
	RequireThrows<std::invalid_argument>([]() { (void)FreshnessPolicy::StaleIfError(0, 1000); },
	                                     "STALE_IF_ERROR with zero fresh was accepted");
	RequireThrows<std::invalid_argument>([]() { (void)FreshnessPolicy::StaleIfError(1000, 0); },
	                                     "STALE_IF_ERROR with zero stale was accepted");
	RequireThrows<std::invalid_argument>(
	    []() { (void)FreshnessPolicy::StaleIfError(1, FreshnessPolicy::MAX_STALE_MILLISECONDS + 1); },
	    "STALE_IF_ERROR over hard stale max was accepted");
}

void TestCacheIdentityBuiltInKeyDimensions() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);

	const auto anon_request =
	    cuac_test::BuildPackageScanRequest(connector, "duckdb_login_search_page", cuac::LogicalSecretReference());
	const auto auth_request = cuac_test::BuildPackageScanRequest(connector, "authenticated_user",
	                                                             cuac::LogicalSecretReference::Named("test_secret"));
	const auto repo_request = cuac_test::BuildPackageScanRequest(connector, "authenticated_repositories",
	                                                             cuac::LogicalSecretReference::Named("test_secret"));
	const auto gql_request = cuac_test::BuildPackageScanRequest(connector, "viewer_repository_metrics",
	                                                            cuac::LogicalSecretReference::Named("test_secret"));

	const auto anon_plan = cuac::BuildConservativeScanPlan(connector, anon_request);
	const auto auth_plan = cuac::BuildConservativeScanPlan(connector, auth_request);
	const auto repo_plan = cuac::BuildConservativeScanPlan(connector, repo_request);
	const auto gql_plan = cuac::BuildConservativeScanPlan(connector, gql_request);

	Require(anon_plan.HasCacheIdentity() && auth_plan.HasCacheIdentity() && repo_plan.HasCacheIdentity() &&
	            gql_plan.HasCacheIdentity(),
	        "plans did not carry a cache identity");

	const auto &anon_id = anon_plan.CacheIdentity();
	const auto &auth_id = auth_plan.CacheIdentity();
	const auto &repo_id = repo_plan.CacheIdentity();
	const auto &gql_id = gql_plan.CacheIdentity();

	Require(anon_id != auth_id && anon_id != repo_id && anon_id != gql_id && auth_id != repo_id && auth_id != gql_id &&
	            repo_id != gql_id,
	        "distinct package relations produced the same cache identity");

	const auto anon_plan_2 = cuac::BuildConservativeScanPlan(connector, anon_request);
	Require(anon_plan_2.CacheIdentity() == anon_id, "same relation produced a non-deterministic cache identity");

	Require(anon_id.Hash() == anon_id.Hash(), "cache identity hash was not deterministic");
	Require(anon_id != anon_id ? false : true, "cache identity did not compare equal to itself");
}

void TestCacheIdentityFreshnessPolicyDoesNotChangeKey() {
	const auto connector = cuac_test::CompileRepositoryGithubConnectorFixture(CUAC_SOURCE_ROOT);
	auto request = cuac_test::BuildPackageScanRequest(connector, "authenticated_user",
	                                                  cuac::LogicalSecretReference::Named("test_secret"));
	const auto plan_off = cuac::BuildConservativeScanPlan(connector, request);

	request.freshness_policy = cuac::FreshnessPolicy::Fresh(60000);
	const auto plan_fresh = cuac::BuildConservativeScanPlan(connector, request);

	Require(plan_off.CacheIdentity() == plan_fresh.CacheIdentity(), "freshness policy changed the cache identity");
	Require(plan_off.Freshness() == cuac::FreshnessPolicy() && plan_fresh.Freshness().IsEnabled(),
	        "plan did not carry the request freshness policy");
}

void TestCacheSemanticIdentityOpaqueness() {
	static_assert(!std::is_default_constructible<cuac::CacheSemanticIdentity>::value,
	              "CacheSemanticIdentity must not be default-constructible");
	static_assert(std::is_copy_constructible<cuac::CacheSemanticIdentity>::value,
	              "CacheSemanticIdentity must be copy-constructible");
	static_assert(!std::is_copy_assignable<cuac::CacheSemanticIdentity>::value,
	              "CacheSemanticIdentity must not be copy-assignable");
	static_assert(!std::is_move_assignable<cuac::CacheSemanticIdentity>::value,
	              "CacheSemanticIdentity must not be move-assignable");
}

} // namespace

int main() {
	try {
		TestFreshnessPolicyDefaults();
		TestFreshnessPolicyFactoriesAndBounds();
		TestFreshnessPolicyRejection();
		TestCacheIdentityBuiltInKeyDimensions();
		TestCacheIdentityFreshnessPolicyDoesNotChangeKey();
		TestCacheSemanticIdentityOpaqueness();
		std::cout << "cache identity tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "cache identity tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
