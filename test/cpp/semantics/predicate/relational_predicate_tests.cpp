#include "cuac/semantics/relational_predicate.hpp"
#include "support/require.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac_test::Require;

cuac::RequestedPredicate VisibilityPrivate(std::size_t column_index) {
	return cuac::RequestedPredicate::Comparison(column_index, cuac::RequestedPredicateValueKind::VARCHAR,
	                                            cuac::RequestedPredicateComparisonOperator::EQUALS,
	                                            cuac::RequestedPredicateValue::Varchar("private"));
}

template <class ACTION>
void RequireInvalid(const ACTION &action, const std::string &message) {
	bool rejected = false;
	try {
		action();
	} catch (const std::invalid_argument &) {
		rejected = true;
	}
	Require(rejected, message);
}

template <class ACTION>
void RequireLogicError(const ACTION &action, const std::string &message) {
	bool rejected = false;
	try {
		action();
	} catch (const std::logic_error &) {
		rejected = true;
	}
	Require(rejected, message);
}

void TestTypedValuesAndSafeSnapshots() {
	const auto bigint = cuac::RequestedPredicateValue::BigInt(-42);
	const auto varchar_value = cuac::RequestedPredicateValue::Varchar("private;\n");
	const auto boolean = cuac::RequestedPredicateValue::Boolean(true);
	const auto timestamptz = cuac::RequestedPredicateValue::Timestamptz(INT64_C(1782864000000000));
	Require(bigint.Kind() == cuac::RequestedPredicateValueKind::BIGINT && bigint.BigIntValue() == -42 &&
	            bigint.Snapshot() == "bigint:-42",
	        "BIGINT predicate value lost its typed identity");
	Require(varchar_value.Kind() == cuac::RequestedPredicateValueKind::VARCHAR &&
	            varchar_value.VarcharValue() == "private;\n" &&
	            varchar_value.Snapshot() == "varchar:hex:707269766174653b0a",
	        "VARCHAR predicate value was not preserved and escaped safely");
	Require(boolean.Kind() == cuac::RequestedPredicateValueKind::BOOLEAN && boolean.BooleanValue() &&
	            boolean.Snapshot() == "boolean:true",
	        "BOOLEAN predicate value lost its typed identity");
	Require(timestamptz.Kind() == cuac::RequestedPredicateValueKind::TIMESTAMPTZ &&
	            timestamptz.TimestamptzMicroseconds() == INT64_C(1782864000000000) &&
	            timestamptz.Snapshot() == "timestamptz:2026-07-01T00:00:00.000000Z",
	        "TIMESTAMPTZ predicate value lost canonical instant identity");
	RequireLogicError([&]() { (void)bigint.VarcharValue(); }, "BIGINT value exposed a VARCHAR payload");
	RequireInvalid([]() { (void)cuac::RequestedPredicateValue::Timestamptz(INT64_C(253402300800000000)); },
	               "out-of-profile TIMESTAMPTZ predicate value was accepted");
	RequireInvalid(
	    []() {
		    (void)cuac::RequestedPredicate::Comparison(0, cuac::RequestedPredicateValueKind::BIGINT,
		                                               cuac::RequestedPredicateComparisonOperator::EQUALS,
		                                               cuac::RequestedPredicateValue::Varchar("1"));
	    },
	    "comparison admitted mismatched column and literal types");
}

void TestCandidateIdentityStructureAndOpaquePositions() {
	const auto unrestricted = cuac::RequestedPredicate();
	const auto comparison = VisibilityPrivate(5);
	const auto unsupported = cuac::RequestedPredicate::Unsupported(7);
	const auto conjunction = cuac::RequestedPredicate::Conjunction({comparison, unsupported});
	const auto disjunction = cuac::RequestedPredicate::Disjunction({comparison, unsupported});
	const auto negation = cuac::RequestedPredicate::Negation(comparison);

	Require(unrestricted == cuac::RequestedPredicate::Unrestricted() &&
	            unrestricted.Kind() == cuac::RequestedPredicateKind::UNRESTRICTED && unrestricted.Snapshot() == "true",
	        "requested predicate default was not conservative TRUE");
	Require(comparison.Kind() == cuac::RequestedPredicateKind::COMPARISON && comparison.BoundColumnIndex() == 5 &&
	            comparison.BoundColumnType() == cuac::RequestedPredicateValueKind::VARCHAR &&
	            comparison.Literal().VarcharValue() == "private" && comparison.Depth() == 1 &&
	            comparison.NodeCount() == 1,
	        "comparison leaf lost its bound typed identity");
	Require(unsupported.Kind() == cuac::RequestedPredicateKind::UNSUPPORTED && unsupported.UnsupportedPosition() == 7 &&
	            unsupported.Snapshot() == "unsupported[position:7]",
	        "opaque unsupported leaf lost its deterministic position");
	Require(conjunction.Kind() == cuac::RequestedPredicateKind::CONJUNCTION && conjunction.Depth() == 2 &&
	            conjunction.NodeCount() == 3 && conjunction.Children().size() == 2 &&
	            conjunction.Snapshot().find("and[") == 0,
	        "conjunction lost ordered child structure or accounting");
	Require(disjunction.Kind() == cuac::RequestedPredicateKind::DISJUNCTION &&
	            disjunction.Children()[0] == comparison && disjunction.Children()[1] == unsupported,
	        "disjunction reordered or rewrote its children");
	Require(negation.Kind() == cuac::RequestedPredicateKind::NEGATION && negation.Children().size() == 1 &&
	            negation.Children()[0] == comparison,
	        "negation lost its sole child");
	Require(conjunction != cuac::RequestedPredicate::Conjunction({unsupported, comparison}),
	        "candidate equality ignored deterministic child order");
	for (const auto &forbidden : {"visibility =", "visibility=private", "SELECT", "WHERE"}) {
		Require(conjunction.Snapshot().find(forbidden) == std::string::npos,
		        "candidate snapshot became SQL or request authority: " + std::string(forbidden));
	}
}

void TestDepthAndNodeBounds() {
	std::vector<cuac::RequestedPredicate> leaves;
	for (std::size_t index = 0; index < cuac::MAX_REQUESTED_PREDICATE_NODES - 1; index++) {
		leaves.push_back(cuac::RequestedPredicate::Unsupported(index));
	}
	const auto maximum = cuac::RequestedPredicate::Conjunction(leaves);
	Require(maximum.NodeCount() == cuac::MAX_REQUESTED_PREDICATE_NODES && maximum.Depth() == 2,
	        "candidate did not admit its exact node ceiling");
	leaves.push_back(cuac::RequestedPredicate::Unsupported(999));
	RequireInvalid([&]() { (void)cuac::RequestedPredicate::Conjunction(leaves); },
	               "candidate admitted more than 64 nodes");

	auto depth = VisibilityPrivate(0);
	for (std::size_t level = 1; level < cuac::MAX_REQUESTED_PREDICATE_DEPTH; level++) {
		depth = cuac::RequestedPredicate::Negation(std::move(depth));
	}
	Require(depth.Depth() == cuac::MAX_REQUESTED_PREDICATE_DEPTH, "candidate did not admit its exact depth ceiling");
	RequireInvalid([&]() { (void)cuac::RequestedPredicate::Negation(depth); }, "candidate admitted depth beyond 16");
	RequireInvalid([]() { (void)cuac::RequestedPredicate::Conjunction({VisibilityPrivate(0)}); },
	               "conjunction admitted fewer than two children");
}

} // namespace

static_assert(std::is_default_constructible<cuac::RequestedPredicate>::value,
              "candidate must default to conservative TRUE");
static_assert(std::is_copy_constructible<cuac::RequestedPredicate>::value,
              "Query must copy immutable candidates into bind state");
static_assert(!std::is_constructible<cuac::RequestedPredicate, std::string>::value,
              "candidate must not admit SQL or arbitrary predicate text");

void RunRelationalPredicateTests() {
	TestTypedValuesAndSafeSnapshots();
	TestCandidateIdentityStructureAndOpaquePositions();
	TestDepthAndNodeBounds();
}
