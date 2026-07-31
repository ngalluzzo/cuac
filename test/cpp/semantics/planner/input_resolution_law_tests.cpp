#include "cuac/internal/semantics/planner/input_resolution.hpp"

#include "connector/support/package_generation_test_fixtures.hpp"
#include "cuac/semantics/scan_planner.hpp"
#include "support/require.hpp"

#include <stdexcept>
#include <string>

namespace {

using cuac::ExplicitInput;
using cuac::ExplicitInputs;
using cuac::ExplicitInputValueKind;
using cuac::PlanningError;
using cuac::PlanningErrorCode;
using cuac::input_resolution::ResolvedInputSource;
using cuac::input_resolution::ResolvedInputState;
using cuac_test::Require;

const cuac::CompiledRelation &TypedRelation(const cuac::CompiledPackageGeneration &generation) {
	const auto *relation = generation.Connector().FindRelation(cuac_test::PACKAGE_TYPED_RELATION);
	if (relation == nullptr) {
		throw std::runtime_error("package fixture lost its typed relation");
	}
	return *relation;
}

template <class Callback>
void RequireInvalidInput(Callback callback, const std::string &counterexample) {
	bool rejected = false;
	try {
		callback();
	} catch (const PlanningError &error) {
		rejected = error.Code() == PlanningErrorCode::INVALID_CONTRACT;
	}
	Require(rejected, "input resolution accepted " + counterexample);
}

void TestOmissionAndTypedDefaults() {
	const auto generation = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto resolved = cuac::input_resolution::ResolveRelationInputs(TypedRelation(generation), ExplicitInputs());
	Require(resolved.Size() == 5, "input resolution changed declaration order or count");

	const auto *query = resolved.Find("query");
	const auto *limit = resolved.Find("limit");
	const auto *include_archived = resolved.Find("include_archived");
	const auto *cursor = resolved.Find("cursor");
	const auto *locale = resolved.Find("locale");
	Require(query != nullptr && query->State() == ResolvedInputState::UNBOUND &&
	            query->Source() == ResolvedInputSource::NONE,
	        "omitted input without a default did not remain UNBOUND");
	Require(limit != nullptr && limit->State() == ResolvedInputState::BOUND_VALUE && limit->BigintValue() == 25 &&
	            limit->Source() == ResolvedInputSource::DEFAULT_VALUE,
	        "omitted BIGINT did not receive its compiled concrete default");
	Require(include_archived != nullptr && include_archived->State() == ResolvedInputState::BOUND_VALUE &&
	            !include_archived->BooleanValue() && include_archived->Source() == ResolvedInputSource::DEFAULT_VALUE,
	        "omitted BOOLEAN did not preserve a false compiled default");
	Require(cursor != nullptr && cursor->State() == ResolvedInputState::BOUND_NULL &&
	            cursor->Source() == ResolvedInputSource::DEFAULT_VALUE,
	        "omitted nullable VARCHAR did not preserve its typed NULL default");
	Require(locale != nullptr && locale->State() == ResolvedInputState::BOUND_VALUE &&
	            locale->VarcharValue() == "global" && locale->Source() == ResolvedInputSource::DEFAULT_VALUE,
	        "omitted nullable VARCHAR did not receive its concrete compiled default");
}

void TestExplicitValuesOverrideDefaultsWithoutCollapsingNull() {
	const auto generation = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto &relation = TypedRelation(generation);
	const auto resolved = cuac::input_resolution::ResolveRelationInputs(
	    relation, ExplicitInputs({ExplicitInput::Varchar("query", ""), ExplicitInput::BigInt("limit", 0),
	                              ExplicitInput::Boolean("include_archived", false),
	                              ExplicitInput::Null("cursor", ExplicitInputValueKind::VARCHAR),
	                              ExplicitInput::Null("locale", ExplicitInputValueKind::VARCHAR)}));
	Require(resolved.Find("query")->State() == ResolvedInputState::BOUND_VALUE &&
	            resolved.Find("query")->VarcharValue().empty() &&
	            resolved.Find("query")->Source() == ResolvedInputSource::EXPLICIT,
	        "empty VARCHAR was collapsed into omission");
	Require(resolved.Find("limit")->BigintValue() == 0 &&
	            resolved.Find("limit")->Source() == ResolvedInputSource::EXPLICIT,
	        "zero BIGINT was replaced by its default");
	Require(!resolved.Find("include_archived")->BooleanValue() &&
	            resolved.Find("include_archived")->Source() == ResolvedInputSource::EXPLICIT,
	        "false BOOLEAN was replaced by its default");
	Require(resolved.Find("cursor")->State() == ResolvedInputState::BOUND_NULL &&
	            resolved.Find("cursor")->Source() == ResolvedInputSource::EXPLICIT,
	        "explicit NULL did not remain distinct from the compiled NULL default");
	Require(resolved.Find("locale")->State() == ResolvedInputState::BOUND_NULL &&
	            resolved.Find("locale")->Source() == ResolvedInputSource::EXPLICIT,
	        "explicit NULL did not suppress the nullable concrete default");

	const auto concrete_cursor = cuac::input_resolution::ResolveRelationInputs(
	    relation, ExplicitInputs({ExplicitInput::Varchar("cursor", "after-token")}));
	Require(concrete_cursor.Find("cursor")->State() == ResolvedInputState::BOUND_VALUE &&
	            concrete_cursor.Find("cursor")->VarcharValue() == "after-token" &&
	            concrete_cursor.Find("cursor")->Source() == ResolvedInputSource::EXPLICIT,
	        "explicit nullable VARCHAR did not override its typed NULL default");
}

void TestInvalidExplicitInputsFailClosed() {
	const auto generation = cuac_test::BuildTypedFallbackPackageGenerationFixture();
	const auto &relation = TypedRelation(generation);
	RequireInvalidInput(
	    [&relation]() {
		    (void)cuac::input_resolution::ResolveRelationInputs(
		        relation, ExplicitInputs({ExplicitInput::Varchar("unknown", "value")}));
	    },
	    "an unknown exact identifier");
	RequireInvalidInput(
	    [&relation]() {
		    (void)cuac::input_resolution::ResolveRelationInputs(
		        relation, ExplicitInputs({ExplicitInput::Boolean("query", true)}));
	    },
	    "a BOOLEAN value for a VARCHAR declaration");
	RequireInvalidInput(
	    [&relation]() {
		    (void)cuac::input_resolution::ResolveRelationInputs(
		        relation, ExplicitInputs({ExplicitInput::Null("query", ExplicitInputValueKind::VARCHAR)}));
	    },
	    "explicit NULL for a non-nullable declaration");

	bool duplicate_rejected = false;
	try {
		(void)ExplicitInputs({ExplicitInput::Varchar("query", "one"), ExplicitInput::Varchar("query", "two")});
	} catch (const std::invalid_argument &) {
		duplicate_rejected = true;
	}
	Require(duplicate_rejected, "the Query handoff admitted duplicate exact explicit input identifiers");
}

} // namespace

void RunInputResolutionLawTests() {
	TestOmissionAndTypedDefaults();
	TestExplicitValuesOverrideDefaultsWithoutCollapsingNull();
	TestInvalidExplicitInputsFailClosed();
}
