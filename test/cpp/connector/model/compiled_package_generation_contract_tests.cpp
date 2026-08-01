#include "cuac/connector/compiled_package_generation.hpp"
#include "cuac/connector/api.hpp"
#include "cuac/internal/connector/model/compiled_model_builder.hpp"
#include "cuac/internal/connector/model/predicate_declaration.hpp"
#include "connector/support/catalog_test_access.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using cuac::CompiledScalarType;
using cuac::internal::CompiledModelBuilder;
using cuac_test::Require;

#define DEFINE_MEMBER_PROBE(PROBE_NAME, MEMBER_NAME)                                                                   \
	template <typename T>                                                                                              \
	class PROBE_NAME {                                                                                                 \
		template <typename U>                                                                                          \
		static char Test(decltype(&U::MEMBER_NAME));                                                                   \
		template <typename U>                                                                                          \
		static long Test(...);                                                                                         \
                                                                                                                       \
	public:                                                                                                            \
		static const bool VALUE = sizeof(Test<T>(0)) == sizeof(char);                                                  \
	}

DEFINE_MEMBER_PROBE(HasConnectorMember, Connector);
DEFINE_MEMBER_PROBE(HasOperationsMember, Operations);
DEFINE_MEMBER_PROBE(HasPredicateMappingsMember, PredicateMappings);
DEFINE_MEMBER_PROBE(HasNetworkPolicyMember, NetworkPolicy);
DEFINE_MEMBER_PROBE(HasExtractorMember, Extractor);
DEFINE_MEMBER_PROBE(HasSnapshotMember, Snapshot);
DEFINE_MEMBER_PROBE(HasUnsupportedOperationSelectorBuilder, OperationSelector);

#undef DEFINE_MEMBER_PROBE

static_assert(std::is_copy_constructible<cuac::CompiledPackageGeneration>::value,
              "package generations must support immutable owner copies");
static_assert(!std::is_copy_assignable<cuac::CompiledPackageGeneration>::value,
              "package generation assignment would replace immutable ownership");
static_assert(!std::is_default_constructible<cuac::CompiledPackageGeneration>::value,
              "package generations must not admit empty ownership");
static_assert(!std::is_default_constructible<cuac::CompiledGenerationHandle>::value,
              "opaque generation handles must originate from validated generations");
static_assert(!std::is_copy_assignable<cuac::CompiledGenerationHandle>::value,
              "opaque generation assignment would replace pinned ownership");
static_assert(!HasConnectorMember<cuac::CompiledQueryRegistrationView>::VALUE,
              "Query registration must not expose the generalized connector");
static_assert(!HasOperationsMember<cuac::CompiledRegistrationRelation>::VALUE,
              "Query registration must not expose operation selection");
static_assert(!HasPredicateMappingsMember<cuac::CompiledRegistrationRelation>::VALUE,
              "Query registration must not expose predicate declarations");
static_assert(!HasNetworkPolicyMember<cuac::CompiledQueryRegistrationView>::VALUE,
              "Query registration must not expose network policy");
static_assert(!HasExtractorMember<cuac::CompiledRegistrationColumn>::VALUE,
              "Query registration must not expose extractor syntax");
static_assert(!HasSnapshotMember<cuac::CompiledGenerationHandle>::VALUE,
              "opaque generation handles must not become a metadata side channel");
static_assert(!HasUnsupportedOperationSelectorBuilder<CompiledModelBuilder>::VALUE,
              "package builder must not accept author priority, alternatives, or forbidden inputs");
static_assert(!std::is_default_constructible<cuac::CompiledRequiredInputReference>::value,
              "tagged required inputs must originate from Connector's validated builder");
static_assert(!std::is_copy_assignable<cuac::CompiledRequiredInputReference>::value,
              "tagged required input assignment would replace immutable authority");

template <class Exception, class Callable>
void RequireThrows(Callable callable, const std::string &message) {
	try {
		callable();
	} catch (const Exception &) {
		return;
	}
	throw std::runtime_error(message);
}

std::string Digest(char digit = 'a') {
	return "sha256." + std::string(64, digit);
}

cuac::CompiledConnector BuildPackageConnector(std::string connector_id = "acme", std::string package_version = "1.2.3",
                                              const std::string *fixed_query = nullptr,
                                              bool changed_structure = false) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("service.example"),
	                                         443};
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id", {"id"}));
	columns.push_back(changed_structure ? CompiledModelBuilder::Column("label", CompiledScalarType::VARCHAR, true,
	                                                                   "$.payload.label", {"payload", "label"})
	                                    : CompiledModelBuilder::Column("label", CompiledScalarType::VARCHAR, true,
	                                                                   "$.label", {"label"}));

	std::vector<cuac::CompiledRelationInput> inputs;
	inputs.push_back(
	    CompiledModelBuilder::Input("enabled", CompiledScalarType::BOOLEAN, false, CompiledModelBuilder::NoDefault()));
	inputs.push_back(CompiledModelBuilder::Input("page_size", CompiledScalarType::BIGINT, false,
	                                             CompiledModelBuilder::Default(CompiledModelBuilder::Bigint(10))));
	inputs.push_back(CompiledModelBuilder::Input(
	    "scope", CompiledScalarType::VARCHAR, true,
	    CompiledModelBuilder::Default(CompiledModelBuilder::Null(CompiledScalarType::VARCHAR))));

	std::vector<cuac::CompiledOperation> operations;
	auto selector = CompiledModelBuilder::V1OperationSelector({});
	std::vector<cuac::CompiledQueryParameter> query;
	if (fixed_query != nullptr) {
		query.push_back(
		    CompiledModelBuilder::FixedQueryParameter("filter", CompiledModelBuilder::Varchar(*fixed_query)));
	}
	operations.push_back(CompiledModelBuilder::RestOperation(
	    "list_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {origin, "/rows", std::move(query), {{"Accept", "application/json"}}},
	    cuac::CompiledResponseSource::JSON_PATH_MANY, changed_structure ? "$.payload.records[*]" : "$.records[*]",
	    changed_structure ? std::vector<std::string>({"payload", "records"}) : std::vector<std::string>({"records"}),
	    std::move(selector)));

	std::vector<cuac::CompiledRelation> relations;
	relations.push_back(CompiledModelBuilder::Relation(
	    "rows", std::move(columns), std::move(inputs), {}, std::move(operations),
	    CompiledModelBuilder::AnonymousAuthentication(), CompiledModelBuilder::UnpaginatedResources(100, 1024)));
	return CompiledModelBuilder::Connector(
	    std::move(connector_id), std::move(package_version), std::move(relations),
	    cuac::CompiledNetworkPolicy {{"https"}, {"service.example"}, false, false, false, false, 65536});
}

cuac::CompiledPackageGeneration BuildGeneration(const std::string &version = "1.2.3", char digest = 'a',
                                                const std::string *fixed_query = nullptr,
                                                bool changed_structure = false) {
	auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "acme", version, Digest(digest));
	return CompiledModelBuilder::PackageGeneration(
	    std::move(identity), BuildPackageConnector("acme", version, fixed_query, changed_structure));
}

cuac::CompiledRelation BuildRelationWithQuery(std::vector<cuac::CompiledQueryParameter> query) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("service.example"),
	                                         443};
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id", {"id"}));
	std::vector<cuac::CompiledOperation> operations;
	operations.push_back(CompiledModelBuilder::RestOperation(
	    "list_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {origin, "/rows", std::move(query), {}}, cuac::CompiledResponseSource::ROOT_ARRAY, "$", {},
	    CompiledModelBuilder::V1OperationSelector({})));
	return CompiledModelBuilder::Relation("rows", std::move(columns), {}, {}, std::move(operations),
	                                      CompiledModelBuilder::AnonymousAuthentication(),
	                                      CompiledModelBuilder::UnpaginatedResources(10, 64));
}

cuac::CompiledRelation BuildRelationWithOperation(cuac::CompiledOperation operation) {
	std::vector<cuac::CompiledColumn> columns;
	columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id", {"id"}));
	std::vector<cuac::CompiledOperation> operations;
	operations.push_back(std::move(operation));
	return CompiledModelBuilder::Relation("rows", std::move(columns), {}, {}, std::move(operations),
	                                      CompiledModelBuilder::AnonymousAuthentication(),
	                                      CompiledModelBuilder::UnpaginatedResources(10, 64));
}

void TestTypedValuesAndDefaultPresence() {
	const auto boolean_value = CompiledModelBuilder::Boolean(true);
	const auto bigint_value = CompiledModelBuilder::Bigint(std::numeric_limits<std::int64_t>::min());
	const auto varchar_value = CompiledModelBuilder::Varchar("");
	const auto null_value = CompiledModelBuilder::Null(CompiledScalarType::VARCHAR);
	auto structural_column = CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id");
	structural_column.logical_type = "VARCHAR";

	Require(boolean_value.Type() == CompiledScalarType::BOOLEAN && !boolean_value.IsNull() && boolean_value.Boolean(),
	        "compiled BOOLEAN value lost its structural payload");
	Require(bigint_value.Type() == CompiledScalarType::BIGINT && !bigint_value.IsNull() &&
	            bigint_value.Bigint() == std::numeric_limits<std::int64_t>::min(),
	        "compiled BIGINT value lost its structural payload");
	Require(varchar_value.Type() == CompiledScalarType::VARCHAR && !varchar_value.IsNull() &&
	            varchar_value.Varchar().empty(),
	        "compiled VARCHAR value confused an empty string with absence");
	Require(null_value.Type() == CompiledScalarType::VARCHAR && null_value.IsNull(),
	        "compiled typed NULL lost its declared type");
	Require(structural_column.ScalarType() == CompiledScalarType::BIGINT,
	        "package column recovered type authority from mutable display text");
	RequireThrows<std::logic_error>([&]() { (void)boolean_value.Bigint(); }, "BOOLEAN value exposed a BIGINT payload");
	RequireThrows<std::logic_error>([&]() { (void)null_value.Varchar(); }, "typed NULL exposed a VARCHAR payload");

	const auto absent = CompiledModelBuilder::NoDefault();
	const auto present_null = CompiledModelBuilder::Default(null_value);
	Require(!absent.HasDefault() && present_null.HasDefault() && present_null.Value().IsNull(),
	        "absent default and present typed NULL default collapsed");
	RequireThrows<std::logic_error>([&]() { (void)absent.Value(); }, "absent default exposed a value");
}

void TestQueryScalarEncodingAndSourceLaws() {
	const auto boolean_value = CompiledModelBuilder::Boolean(true);
	const auto bigint_value = CompiledModelBuilder::Bigint(-42);
	const auto varchar_value = CompiledModelBuilder::Varchar(u8"a b/é");
	Require(cuac::EncodeCompiledQueryScalar(boolean_value) == "true" &&
	            cuac::EncodeCompiledQueryScalar(bigint_value) == "-42" &&
	            cuac::EncodeCompiledQueryScalar(varchar_value) == "a+b%2F%C3%A9" &&
	            cuac::EncodeCompiledQueryScalar(CompiledModelBuilder::Varchar("")) == "",
	        "typed query scalar encoding drifted from canonical FORM_URLENCODED bytes");
	RequireThrows<std::invalid_argument>(
	    []() { (void)cuac::EncodeCompiledQueryScalar(CompiledModelBuilder::Null(CompiledScalarType::VARCHAR)); },
	    "typed NULL acquired query bytes instead of omission ownership");

	const std::vector<std::string> invalid_text = {std::string("a\0b", 3),
	                                               std::string("a") + static_cast<char>(0x1f) + "b",
	                                               std::string("a") + static_cast<char>(0x7f) + "b",
	                                               std::string("\xc2\x80", 2),
	                                               std::string("\xc0\xaf", 2),
	                                               std::string("\xed\xa0\x80", 3),
	                                               std::string("\xf4\x90\x80\x80", 4)};
	for (const auto &value : invalid_text) {
		RequireThrows<std::invalid_argument>(
		    [&value]() { (void)cuac::EncodeCompiledQueryScalar(CompiledModelBuilder::Varchar(value)); },
		    "invalid, NUL, or control-bearing UTF-8 acquired query bytes");
	}

	const auto fixed = CompiledModelBuilder::FixedQueryParameter("filter", CompiledModelBuilder::Varchar(u8"a b/é"));
	const auto relation = CompiledModelBuilder::RelationInputQueryParameter("q", "search");
	const auto conditional = CompiledModelBuilder::ConditionalInputQueryParameter("visibility", "visibility");
	const auto page_size = CompiledModelBuilder::PageSizeQueryParameter("batch", 100);
	const auto page_number = CompiledModelBuilder::PageNumberQueryParameter("cursor_page", 1);
	Require(fixed.source == cuac::CompiledQueryValueSource::FIXED && fixed.source_id.empty() &&
	            fixed.encoding == cuac::CompiledQueryEncoding::FORM_URLENCODED && fixed.HasDecodedValue() &&
	            fixed.DecodedValue().Type() == CompiledScalarType::VARCHAR &&
	            fixed.DecodedValue().Varchar() == u8"a b/é" && fixed.encoded_value == "a+b%2F%C3%A9" &&
	            !fixed.omit_when_unbound && !fixed.omit_when_null,
	        "fixed query field lost decoded VARCHAR, encoded bytes, or closed omission policy");
	Require(relation.source == cuac::CompiledQueryValueSource::RELATION_INPUT && relation.source_id == "search" &&
	            !relation.HasDecodedValue() && relation.encoded_value.empty() && relation.omit_when_unbound &&
	            relation.omit_when_null,
	        "relation-input query field lost source identity or omission policy");
	Require(conditional.source == cuac::CompiledQueryValueSource::CONDITIONAL_INPUT &&
	            conditional.source_id == "visibility" && !conditional.HasDecodedValue() &&
	            conditional.encoded_value.empty() && conditional.omit_when_unbound && !conditional.omit_when_null,
	        "conditional query field lost source identity or omission policy");
	Require(page_size.source == cuac::CompiledQueryValueSource::PAGE_SIZE && page_size.source_id.empty() &&
	            page_size.HasDecodedValue() && page_size.DecodedValue().Type() == CompiledScalarType::BIGINT &&
	            page_size.DecodedValue().Bigint() == 100 && page_size.encoded_value == "100" &&
	            !page_size.omit_when_unbound && !page_size.omit_when_null &&
	            page_number.source == cuac::CompiledQueryValueSource::PAGE_NUMBER &&
	            page_number.DecodedValue().Bigint() == 1,
	        "page query fields lost closed roles or structural BIGINT initial values");
	RequireThrows<std::invalid_argument>([]() { (void)CompiledModelBuilder::PageSizeQueryParameter("page_size", 0); },
	                                     "zero page size entered a structural query field");

	auto mismatched = fixed;
	mismatched.encoded_value = "a%20b";
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({mismatched}); },
	                                     "fixed query bytes disagreed with their decoded scalar");
	auto contradictory_relation = relation;
	contradictory_relation.omit_when_null = false;
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({contradictory_relation}); },
	                                     "relation-input query accepted conditional omission policy");
	auto contradictory_conditional = conditional;
	contradictory_conditional.source_id.clear();
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({contradictory_conditional}); },
	                                     "conditional query accepted an absent source identifier");
	auto wrong_source = fixed;
	wrong_source.source = cuac::CompiledQueryValueSource::RELATION_INPUT;
	wrong_source.source_id = "search";
	wrong_source.omit_when_unbound = true;
	wrong_source.omit_when_null = true;
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({wrong_source}); },
	                                     "relation-input tag retained a contradictory concrete scalar");
	auto unknown_source = fixed;
	unknown_source.source = static_cast<cuac::CompiledQueryValueSource>(127);
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({unknown_source}); },
	                                     "unknown query source entered a validated operation");
	auto unknown_encoding = fixed;
	unknown_encoding.encoding = static_cast<cuac::CompiledQueryEncoding>(127);
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({unknown_encoding}); },
	                                     "unknown query encoding entered a validated operation");
	for (const auto &name : {std::string(""), std::string(64, 'a'), std::string("bad/name"),
	                         std::string("bad\0name", 8), std::string(u8"é")}) {
		auto invalid_name = fixed;
		invalid_name.name = name;
		RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({invalid_name}); },
		                                     "query name outside exact ASCII grammar entered a validated operation");
	}
	std::vector<cuac::CompiledQueryParameter> excessive_query;
	for (std::size_t index = 0; index < 65; index++) {
		excessive_query.push_back(CompiledModelBuilder::FixedQueryParameter("field" + std::to_string(index),
		                                                                    CompiledModelBuilder::Varchar("value")));
	}
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery(std::move(excessive_query)); },
	                                     "65 query fields entered immutable compiled IR");
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithQuery({page_size}); },
	                                     "disabled pagination accepted a structural page role");
}

void TestStructuralExtractorLaws() {
	const auto nested =
	    CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.record.id", {"record", "id"});
	Require(nested.ExtractorSegments() == std::vector<std::string>({"record", "id"}),
	        "column lost its decoded extractor segments");
	for (const auto &segment : {std::string("items[0]"), std::string("items[*]"), std::string("bad.segment"),
	                            std::string("9records"), std::string("records\n")}) {
		RequireThrows<std::invalid_argument>(
		    [&segment]() {
			    (void)CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$." + segment, {segment});
		    },
		    "invalid structural column segment escaped Connector validation");
	}

	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("service.example"),
	                                         443};
	auto records_operation = CompiledModelBuilder::RestOperation(
	    "list_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {origin, "/rows", {}, {}}, cuac::CompiledResponseSource::JSON_PATH_MANY, "$.records[*]", {"records"},
	    CompiledModelBuilder::V1OperationSelector({}));
	const auto relation = BuildRelationWithOperation(std::move(records_operation));
	Require(relation.Operation().Rest().records_extractor_segments == std::vector<std::string>({"records"}),
	        "REST operation lost its terminal collection segments");

	const auto require_invalid_records = [&origin](std::string extractor, std::vector<std::string> segments) {
		auto operation =
		    CompiledModelBuilder::RestOperation("list_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY,
		                                        CompiledModelBuilder::DisabledPagination(), {origin, "/rows", {}, {}},
		                                        cuac::CompiledResponseSource::JSON_PATH_MANY, std::move(extractor),
		                                        std::move(segments), CompiledModelBuilder::V1OperationSelector({}));
		(void)BuildRelationWithOperation(std::move(operation));
	};
	RequireThrows<std::invalid_argument>([&]() { require_invalid_records("$.records[*]", {"other"}); },
	                                     "record extractor disagreed with its structural segments");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    require_invalid_records("$.items[*].records[*]", {"items[*]", "records"});
	    },
	    "interior array syntax entered structural record segments");
	RequireThrows<std::invalid_argument>([&]() { require_invalid_records("$.records\n[*]", {"records\n"}); },
	                                     "control-bearing structural record segment was accepted");

	std::vector<std::string> exact_segments(511, "a");
	exact_segments.back() = "aa";
	std::string exact_field = "$";
	for (const auto &segment : exact_segments) {
		exact_field += "." + segment;
	}
	Require(exact_field.size() == 1024 && exact_segments.size() > 16, "deep REST path boundary fixture is not exact");
	const auto exact_column =
	    CompiledModelBuilder::Column("deep", CompiledScalarType::VARCHAR, false, exact_field, exact_segments);
	Require(exact_column.ExtractorSegments() == exact_segments,
	        "exact 1024-byte REST column path or its deep structural segments were rejected");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    auto one_over_segments = exact_segments;
		    one_over_segments.back() = "aaa";
		    (void)CompiledModelBuilder::Column("deep", CompiledScalarType::VARCHAR, false, exact_field + "a",
		                                       std::move(one_over_segments));
	    },
	    "1025-byte REST column extractor entered immutable compiled IR");

	const auto exact_collection = exact_field + "[*]";
	auto deep_records = CompiledModelBuilder::RestOperation(
	    "deep_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {origin, "/deep-rows", {}, {}}, cuac::CompiledResponseSource::JSON_PATH_MANY, exact_collection, exact_segments,
	    CompiledModelBuilder::V1OperationSelector({}));
	const auto deep_relation = BuildRelationWithOperation(std::move(deep_records));
	Require(exact_collection.size() == 1027 &&
	            deep_relation.Operation().Rest().records_extractor_segments == exact_segments,
	        "exact 1027-byte deep REST collection path was rejected or truncated");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    auto one_over_segments = exact_segments;
		    one_over_segments.back() = "aaa";
		    require_invalid_records(exact_field + "a[*]", std::move(one_over_segments));
	    },
	    "1028-byte REST collection extractor entered immutable compiled IR");

	auto root_with_segments = CompiledModelBuilder::RestOperation(
	    "list_rows", true, cuac::CompiledOperationCardinality::ZERO_TO_MANY, CompiledModelBuilder::DisabledPagination(),
	    {origin, "/rows", {}, {}}, cuac::CompiledResponseSource::ROOT_ARRAY, "$", {"records"},
	    CompiledModelBuilder::V1OperationSelector({}));
	RequireThrows<std::invalid_argument>([&]() { (void)BuildRelationWithOperation(std::move(root_with_segments)); },
	                                     "root response retained contradictory extractor segments");
}

void TestGenerationSnapshotSensitivity() {
	const std::string active_value = u8"a b/é";
	const std::string changed_value = "a+b";
	const auto active = BuildGeneration("1.2.3", 'a', &active_value);
	const auto candidate = BuildGeneration("1.3.0", 'b', &changed_value);
	Require(active.Connector().Relations()[0].Snapshot().find("filter=fixed.VARCHAR:a+b%2F%C3%A9") !=
	                std::string::npos &&
	            active.Connector().Relations()[0].Snapshot() != candidate.Connector().Relations()[0].Snapshot(),
	        "generation snapshot omitted decoded fixed-query identity or canonical bytes");
	const auto structural_candidate = BuildGeneration("1.3.0", 'c', &active_value, true);
	const auto &active_relation = active.Connector().Relations()[0];
	const auto &changed_relation = structural_candidate.Connector().Relations()[0];
	Require(active_relation.Columns()[1].ExtractorSegments() == std::vector<std::string>({"label"}) &&
	            changed_relation.Columns()[1].ExtractorSegments() == std::vector<std::string>({"payload", "label"}) &&
	            active_relation.Operation().Rest().records_extractor_segments ==
	                std::vector<std::string>({"records"}) &&
	            changed_relation.Operation().Rest().records_extractor_segments ==
	                std::vector<std::string>({"payload", "records"}) &&
	            active_relation.Snapshot() != changed_relation.Snapshot(),
	        "generation snapshot ignored structural record or column extractor segments");
}

void TestInputValidationAndOrdering() {
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)CompiledModelBuilder::Input("secret", CompiledScalarType::VARCHAR, true,
		                                      CompiledModelBuilder::NoDefault());
	    },
	    "reserved Query input name was accepted");
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)CompiledModelBuilder::Input(std::string(64, 'a'), CompiledScalarType::VARCHAR, true,
		                                      CompiledModelBuilder::NoDefault());
	    },
	    "overlong v1 input identifier was accepted");
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)CompiledModelBuilder::Input(
		        "count", CompiledScalarType::BIGINT, false,
		        CompiledModelBuilder::Default(CompiledModelBuilder::Null(CompiledScalarType::BIGINT)));
	    },
	    "non-nullable input accepted a NULL default");
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)CompiledModelBuilder::Input("count", CompiledScalarType::BIGINT, true,
		                                      CompiledModelBuilder::Default(CompiledModelBuilder::Varchar("10")));
	    },
	    "input accepted a default of another structural type");

	const auto generation = BuildGeneration();
	const auto &inputs = generation.Connector().Relations().at(0).Inputs();
	Require(inputs.size() == 3 && inputs[0].Name() == "enabled" && inputs[1].Name() == "page_size" &&
	            inputs[2].Name() == "scope",
	        "compiled relation did not preserve declared input order");
	Require(!inputs[0].Default().HasDefault() && inputs[1].Default().Value().Bigint() == 10 && inputs[2].Nullable() &&
	            inputs[2].Default().Value().IsNull(),
	        "compiled relation lost default presence, value, or nullability");
}

cuac::CompiledOperation SelectorTestOperation(cuac::CompiledOperationSelector selector, bool fallback = false) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS, cuac::CompiledHttpHost("service.example"),
	                                         443};
	return cuac::CompiledOperation {"selected_rows",
	                                fallback,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                CompiledModelBuilder::DisabledPagination(),
	                                {origin, "/selected", {}, {}},
	                                cuac::CompiledResponseSource::ROOT_ARRAY,
	                                "$",
	                                std::move(selector)};
}

cuac::CompiledOperation ControlledExactOperation(cuac::CompiledOperationSelector selector) {
	const cuac::CompiledHttpOrigin origin = {cuac::CompiledUrlScheme::HTTPS,
	                                         cuac::CompiledHttpHost("predicate-proof.invalid"), 443};
	return cuac::CompiledOperation {"controlled_exact_repositories",
	                                false,
	                                cuac::CompiledOperationCardinality::ZERO_TO_MANY,
	                                cuac::CompiledProtocol::REST,
	                                cuac::CompiledHttpMethod::GET,
	                                cuac::CompiledReplaySafety::SAFE,
	                                false,
	                                CompiledModelBuilder::DisabledPagination(),
	                                {origin,
	                                 "/fixtures/exact-repositories",
	                                 {CompiledModelBuilder::ConditionalInputQueryParameter("visibility", "visibility")},
	                                 {{"X-Connector-Fixture", "exact-duplicate-repositories"}}},
	                                cuac::CompiledResponseSource::ROOT_ARRAY,
	                                "$",
	                                std::move(selector)};
}

void TestTaggedRequiredInputReferences() {
	const auto relation_reference = CompiledModelBuilder::RelationInputReference("query");
	const auto conditional_reference = CompiledModelBuilder::ConditionalInputReference("visibility");
	Require(relation_reference.Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT &&
	            relation_reference.Id() == "query" &&
	            conditional_reference.Kind() == cuac::CompiledRequiredInputKind::CONDITIONAL_INPUT &&
	            conditional_reference.Id() == "visibility",
	        "compiled required-input tags or exact identifiers drifted");
	RequireThrows<std::invalid_argument>(
	    []() { (void)CompiledModelBuilder::RelationInputReference("input.query"); },
	    "compiled reference parsed an author prefix instead of accepting an exact identifier");
	RequireThrows<std::invalid_argument>(
	    []() { (void)CompiledModelBuilder::ConditionalInputReference(std::string(64, 'a')); },
	    "compiled reference accepted an overlong identifier");
	RequireThrows<std::invalid_argument>(
	    []() {
		    (void)CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("query"),
		                                                     CompiledModelBuilder::RelationInputReference("query")});
	    },
	    "v1 selector accepted a duplicate tagged reference");

	const auto canonical = CompiledModelBuilder::V1OperationSelector(
	    {CompiledModelBuilder::ConditionalInputReference("zeta"), CompiledModelBuilder::RelationInputReference("zeta"),
	     CompiledModelBuilder::RelationInputReference("alpha")});
	Require(canonical.RequiredInputReferences().size() == 3 &&
	            canonical.RequiredInputReferences()[0].Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT &&
	            canonical.RequiredInputReferences()[0].Id() == "alpha" &&
	            canonical.RequiredInputReferences()[1].Kind() == cuac::CompiledRequiredInputKind::RELATION_INPUT &&
	            canonical.RequiredInputReferences()[1].Id() == "zeta" &&
	            canonical.RequiredInputReferences()[2].Kind() == cuac::CompiledRequiredInputKind::CONDITIONAL_INPUT &&
	            canonical.RequiredInputReferences()[2].Id() == "zeta",
	        "v1 selector failed canonical tagged ordering");

	const auto make_relation = [](cuac::CompiledOperationSelector selector, bool fallback) {
		std::vector<cuac::CompiledColumn> columns;
		columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id"));
		std::vector<cuac::CompiledRelationInput> inputs;
		inputs.push_back(CompiledModelBuilder::Input("query", CompiledScalarType::VARCHAR, false,
		                                             CompiledModelBuilder::NoDefault()));
		std::vector<cuac::CompiledOperation> operations;
		operations.push_back(SelectorTestOperation(std::move(selector), fallback));
		return CompiledModelBuilder::Relation("selected", std::move(columns), std::move(inputs), {},
		                                      std::move(operations), CompiledModelBuilder::AnonymousAuthentication(),
		                                      CompiledModelBuilder::UnpaginatedResources(8, 64));
	};
	const auto relation = make_relation(
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("query")}), false);
	Require(relation.Operations()[0].selector.RequiredInputReferences()[0].Id() == "query",
	        "exact tagged relation input did not survive relation validation");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)make_relation(
		        CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::ConditionalInputReference("query")}),
		        false);
	    },
	    "wrong-kind conditional reference matched a relation input with the same identifier");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)make_relation(
		        CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("missing")}),
		        false);
	    },
	    "missing tagged relation input escaped validation");
	RequireThrows<std::invalid_argument>(
	    [&]() { (void)make_relation(CompiledModelBuilder::V1OperationSelector({}), false); },
	    "selected v1 operation accepted an absent when.required_inputs declaration");
	RequireThrows<std::invalid_argument>(
	    [&]() {
		    (void)make_relation(
		        CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::RelationInputReference("query")}),
		        true);
	    },
	    "fallback v1 operation accepted a when.required_inputs declaration");
	RequireThrows<std::invalid_argument>(
	    []() {
		    std::vector<cuac::CompiledRelationInput> inputs;
		    std::vector<cuac::CompiledRequiredInputReference> references;
		    for (std::size_t index = 0; index < 129; index++) {
			    const auto id = "input_" + std::to_string(index);
			    inputs.push_back(CompiledModelBuilder::Input(id, CompiledScalarType::VARCHAR, false,
			                                                 CompiledModelBuilder::NoDefault()));
			    references.push_back(CompiledModelBuilder::RelationInputReference(id));
		    }
		    std::vector<cuac::CompiledColumn> columns;
		    columns.push_back(CompiledModelBuilder::Column("id", CompiledScalarType::BIGINT, false, "$.id"));
		    std::vector<cuac::CompiledOperation> operations;
		    operations.push_back(
		        SelectorTestOperation(CompiledModelBuilder::V1OperationSelector(std::move(references))));
		    (void)CompiledModelBuilder::Relation("selected", std::move(columns), std::move(inputs), {},
		                                         std::move(operations), CompiledModelBuilder::AnonymousAuthentication(),
		                                         CompiledModelBuilder::UnpaginatedResources(8, 64));
	    },
	    "v1 selector accepted more than 128 required-input references");

	std::vector<cuac::CompiledColumn> conditional_columns;
	conditional_columns.push_back(
	    CompiledModelBuilder::Column("occurrence_id", CompiledScalarType::BIGINT, false, "$.occurrence_id"));
	conditional_columns.push_back(
	    CompiledModelBuilder::Column("visibility", CompiledScalarType::VARCHAR, false, "$.visibility"));
	std::vector<cuac::CompiledRelationInput> colliding_inputs;
	colliding_inputs.push_back(CompiledModelBuilder::Input("visibility", CompiledScalarType::VARCHAR, false,
	                                                       CompiledModelBuilder::NoDefault()));
	std::vector<cuac::CompiledOperation> conditional_operations;
	conditional_operations.push_back(ControlledExactOperation(
	    CompiledModelBuilder::V1OperationSelector({CompiledModelBuilder::ConditionalInputReference("visibility")})));
	const auto identities = cuac::internal::DerivePackagePredicateIdentities(Digest(), "controlled_exact_repositories",
	                                                                         conditional_operations.front());
	std::vector<cuac::CompiledPredicateMapping> mappings;
	mappings.push_back(cuac_test::ConnectorCatalogTestAccess::PackagePredicateMapping(
	    "visibility", CompiledModelBuilder::Varchar("private"), "controlled_exact_repositories", "visibility",
	    "private", cuac::CompiledPredicateAccuracy::EXACT, identities.proof, identities.base_domain, "private_match",
	    "private_false_or_null", "private_duplicates"));
	const auto conditional_relation = CompiledModelBuilder::Relation(
	    "controlled_exact_repositories", std::move(conditional_columns), std::move(colliding_inputs),
	    std::move(mappings), std::move(conditional_operations), CompiledModelBuilder::AnonymousAuthentication(),
	    CompiledModelBuilder::UnpaginatedResources(8, 128));
	Require(conditional_relation.Operations()[0].selector.RequiredInputReferences()[0].Kind() ==
	            cuac::CompiledRequiredInputKind::CONDITIONAL_INPUT,
	        "exact conditional tag did not validate against its operation-local mapping when the relation namespace "
	        "collided");
}

void TestStableIdentityValidation() {
	const auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "acme", "1.2.3", Digest());
	Require(identity.SpecIdentifier() == "cuac/v1" && identity.ConnectorId() == "acme" &&
	            identity.PackageVersion() == "1.2.3" && identity.PackageDigest() == Digest(),
	        "compiled package identity changed accepted bytes");

	for (const auto &spec : {"", "cuac/draft", "cuac/unsupported"}) {
		RequireThrows<std::invalid_argument>(
		    [spec]() { (void)CompiledModelBuilder::PackageIdentity(spec, "acme", "1.2.3", Digest()); },
		    "unsupported spec identifier entered a package generation");
	}
	for (const auto &version : {"1", "1.2", "01.2.3", "1.2.3-alpha", "1.2.3+build", "4294967296.0.0"}) {
		RequireThrows<std::invalid_argument>(
		    [version]() { (void)CompiledModelBuilder::PackageIdentity("cuac/v1", "acme", version, Digest()); },
		    "non-canonical package version entered a package generation");
	}
	const std::vector<std::string> invalid_digests = {"", "sha256.abc", "sha256." + std::string(64, 'A'),
	                                                  "sha512." + std::string(64, 'a')};
	for (const auto &digest : invalid_digests) {
		RequireThrows<std::invalid_argument>(
		    [digest]() { (void)CompiledModelBuilder::PackageIdentity("cuac/v1", "acme", "1.2.3", digest); },
		    "invalid package digest entered a package generation");
	}

	RequireThrows<std::invalid_argument>(
	    []() {
		    auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "other", "1.2.3", Digest());
		    (void)CompiledModelBuilder::PackageGeneration(std::move(identity), BuildPackageConnector());
	    },
	    "generation accepted a connector-identity mismatch");
	RequireThrows<std::invalid_argument>(
	    []() {
		    auto identity = CompiledModelBuilder::PackageIdentity("cuac/v1", "acme", "1.2.4", Digest());
		    (void)CompiledModelBuilder::PackageGeneration(std::move(identity), BuildPackageConnector());
	    },
	    "generation accepted a package-version mismatch");
}

void TestQueryProjectionAndLifetime() {
	const auto generation = BuildGeneration();
	const auto generation_copy = generation;
	const auto view = generation.QueryRegistration();
	Require(&generation.Connector() == &generation_copy.Connector(),
	        "generation copy duplicated rather than shared immutable ownership");
	Require(view.Identity().ConnectorId() == "acme" && view.Relations().size() == 1,
	        "Query registration lost package identity or relation order");
	const auto &relation = view.Relations().at(0);
	Require(relation.Name() == "rows" &&
	            relation.Authentication() == cuac::CompiledRegistrationAuthentication::ANONYMOUS,
	        "Query registration changed relation identity or auth shape");
	Require(relation.Columns().size() == 2 && relation.Columns()[0].Name() == "id" &&
	            relation.Columns()[0].Type() == CompiledScalarType::BIGINT && !relation.Columns()[0].Nullable() &&
	            relation.Columns()[1].Name() == "label" &&
	            relation.Columns()[1].Type() == CompiledScalarType::VARCHAR && relation.Columns()[1].Nullable(),
	        "Query registration did not preserve structural output order and nullability");
	Require(relation.Inputs().size() == 3 && relation.Inputs()[1].Default().Value().Bigint() == 10,
	        "Query registration did not preserve structural input defaults");
	Require(view.GenerationHandle().IsValid() && view.GenerationHandle().IsSameGeneration(generation.OpaqueHandle()) &&
	            !view.GenerationHandle().IsSameGeneration(BuildGeneration().OpaqueHandle()),
	        "opaque handle did not distinguish exact immutable generations");

	const auto retained_view = []() {
		return BuildGeneration().QueryRegistration();
	}();
	Require(retained_view.GenerationHandle().IsValid() && retained_view.Relations().at(0).Name() == "rows",
	        "Query view did not retain its generation after the provider owner ended");
}

} // namespace

int main() {
	try {
		TestTypedValuesAndDefaultPresence();
		TestQueryScalarEncodingAndSourceLaws();
		TestStructuralExtractorLaws();
		TestInputValidationAndOrdering();
		TestTaggedRequiredInputReferences();
		TestStableIdentityValidation();
		TestQueryProjectionAndLifetime();
		TestGenerationSnapshotSensitivity();
		std::cout << "compiled package generation contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "compiled package generation contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
