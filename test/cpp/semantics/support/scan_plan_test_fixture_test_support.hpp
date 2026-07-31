#ifndef CUAC_TEST_SCAN_PLAN_TEST_FIXTURE_TEST_SUPPORT_HPP
#define CUAC_TEST_SCAN_PLAN_TEST_FIXTURE_TEST_SUPPORT_HPP

#include "cuac/semantics/scan_plan.hpp"

#include <string>

namespace cuac_test {
namespace scan_plan_fixture_contract {

void RequireCanaryAbsent(const cuac::ScanPlan &plan, const std::string &canary);

void TestOperationCounterexamples(const std::string &canary);
void TestAuthenticationCounterexamples(const std::string &canary);
void TestResponseCounterexamples(const std::string &canary);
void TestNetworkCounterexamples(const std::string &canary);
void TestFeatureCounterexamples(const std::string &canary);
void TestResourceCounterexamples(const std::string &canary);
void TestRestQueryPathFixture(const std::string &canary);
void TestSafeConsumerHeaderBoundary();

} // namespace scan_plan_fixture_contract
} // namespace cuac_test

#endif
