#include "cuac/internal/connector/fixtures/package_fixture_index.hpp"

#include "cuac/internal/connector/fixtures/package_fixture_index_parser.hpp"
#include "cuac/internal/connector/fixtures/package_fixture_limits.hpp"

#include <set>
#include <stdexcept>
#include <utility>

namespace cuac {
namespace connector {
namespace internal {
namespace {

using namespace fixture_index_detail;

PackageFixtureCase ParseCase(const FailsafeYamlNode &node, std::size_t case_index,
                             const CompiledPackageGeneration &generation, const PackageFixtureLimits &limits,
                             std::vector<std::pair<std::string, std::string>> &payloads) {
	const auto path = "$.cases[" + std::to_string(case_index) + "]";
	ClosedMapping(node,
	              {"id", "relation", "operation", "covers", "inputs", "predicate", "auth", "execution", "proof",
	               "pre_request_failure", "expected"},
	              path);
	const auto id = Scalar(Required(node, "id", path), path + ".id");
	const auto relation_id = Scalar(Required(node, "relation", path), path + ".relation");
	const auto operation_id = Scalar(Required(node, "operation", path), path + ".operation");
	if (!IsId(id) || !IsId(relation_id) || !IsId(operation_id)) {
		Fail(node, path, "fixture case identity is invalid");
	}
	const auto *relation = generation.Connector().FindRelation(relation_id);
	const auto *operation = relation == nullptr ? nullptr : FindOperation(*relation, operation_id);
	if (relation == nullptr || operation == nullptr) {
		Fail(node, path, "fixture case references an unknown relation or operation", id, relation_id, operation_id);
	}

	const auto &covers = Required(node, "covers", path);
	RequireType(covers, FailsafeYamlNode::Kind::SEQUENCE, path + ".covers");
	if (covers.Size() == 0) {
		Fail(covers, path + ".covers", "fixture case must claim at least one coverage key", id, relation_id,
		     operation_id);
	}
	std::set<std::string> unique_covers;
	std::vector<std::string> parsed_covers;
	for (std::size_t index = 0; index < covers.Size(); index++) {
		const auto value = Scalar(covers.SequenceValue(index), path + ".covers[" + std::to_string(index) + "]");
		if (!IsId(value, 255) || !unique_covers.insert(value).second) {
			Fail(covers.SequenceValue(index), path + ".covers[" + std::to_string(index) + "]",
			     "fixture coverage key is invalid or repeated", id, relation_id, operation_id);
		}
		parsed_covers.push_back(value);
	}

	const auto &inputs = Required(node, "inputs", path);
	RequireType(inputs, FailsafeYamlNode::Kind::SEQUENCE, path + ".inputs");
	if (inputs.Size() > 128) {
		Fail(inputs, path + ".inputs", "fixture input budget is exhausted", id, relation_id, operation_id,
		     FixtureIndexFailureKind::RESOURCE_EXHAUSTED);
	}
	std::set<std::string> unique_inputs;
	std::vector<PackageFixtureInput> parsed_inputs;
	for (std::size_t index = 0; index < inputs.Size(); index++) {
		const auto &item = inputs.SequenceValue(index);
		const auto item_path = path + ".inputs[" + std::to_string(index) + "]";
		ClosedMapping(item, {"id", "value"}, item_path);
		const auto input_id = Scalar(Required(item, "id", item_path), item_path + ".id");
		const auto *input = FindInput(*relation, input_id);
		auto value = ParseValue(Required(item, "value", item_path), item_path + ".value");
		if (input == nullptr || !unique_inputs.insert(input_id).second || (value.is_null && !input->Nullable()) ||
		    (!value.is_null && !IsTypedScalar(input->Type(), value.value))) {
			Fail(item, item_path, "fixture input is unknown, repeated, null-invalid, or type-invalid", id, relation_id,
			     operation_id);
		}
		parsed_inputs.push_back({input_id, std::move(value)});
	}

	const bool has_predicate = node.Find("predicate") != nullptr;
	PackageFixturePredicate predicate = {"", CompiledScalarType::VARCHAR, ""};
	if (has_predicate) {
		const auto &source = *node.Find("predicate");
		ClosedMapping(source, {"column", "operator", "literal"}, path + ".predicate");
		if (Scalar(Required(source, "operator", path + ".predicate"), path + ".predicate.operator") != "eq") {
			Fail(source, path + ".predicate", "fixture predicate operator is invalid", id, relation_id, operation_id);
		}
		const auto column_name = Scalar(Required(source, "column", path + ".predicate"), path + ".predicate.column");
		const auto *column = FindColumn(*relation, column_name);
		const auto &literal = Required(source, "literal", path + ".predicate");
		ClosedMapping(literal, {"type", "value"}, path + ".predicate.literal");
		const auto type =
		    Scalar(Required(literal, "type", path + ".predicate.literal"), path + ".predicate.literal.type");
		const auto value =
		    Scalar(Required(literal, "value", path + ".predicate.literal"), path + ".predicate.literal.value");
		const auto scalar_type = type == "BOOLEAN"       ? CompiledScalarType::BOOLEAN
		                         : type == "BIGINT"      ? CompiledScalarType::BIGINT
		                         : type == "DOUBLE"      ? CompiledScalarType::DOUBLE
		                         : type == "TIMESTAMPTZ" ? CompiledScalarType::TIMESTAMPTZ
		                                                 : CompiledScalarType::VARCHAR;
		if (column == nullptr || column->Shape() != CompiledColumnShape::SCALAR ||
		    (type != "BOOLEAN" && type != "BIGINT" && type != "VARCHAR" && type != "DOUBLE" && type != "TIMESTAMPTZ") ||
		    column->ScalarType() != scalar_type || !IsTypedScalar(scalar_type, value)) {
			Fail(source, path + ".predicate", "fixture predicate is incompatible with the compiled column", id,
			     relation_id, operation_id);
		}
		predicate = {column_name, scalar_type, value};
	}

	const auto auth = Scalar(Required(node, "auth", path), path + ".auth");
	PackageFixtureAuthentication authentication;
	if (auth == "anonymous") {
		authentication = PackageFixtureAuthentication::ANONYMOUS;
	} else if (auth == "bearer_present") {
		authentication = PackageFixtureAuthentication::BEARER_PRESENT;
	} else if (auth == "bearer_missing") {
		authentication = PackageFixtureAuthentication::BEARER_MISSING;
	} else {
		Fail(node, path, "fixture authentication is outside the closed vocabulary", id, relation_id, operation_id);
	}

	PackageFixtureTranscriptKind transcript_kind;
	std::vector<PackageFixturePage> pages;
	std::vector<PackageFixturePage> restricted_pages;
	const auto alternatives = static_cast<unsigned>(node.Find("execution") != nullptr) +
	                          static_cast<unsigned>(node.Find("proof") != nullptr) +
	                          static_cast<unsigned>(node.Find("pre_request_failure") != nullptr);
	if (alternatives != 1) {
		Fail(node, path, "fixture case must contain exactly one transcript alternative", id, relation_id, operation_id);
	}
	if (node.Find("execution")) {
		transcript_kind = PackageFixtureTranscriptKind::EXECUTION;
		pages = ParsePages(*node.Find("execution"), path + ".execution", false, limits, payloads);
	} else if (node.Find("proof")) {
		if (!has_predicate) {
			Fail(node, path, "predicate proof transcript requires a predicate", id, relation_id, operation_id);
		}
		transcript_kind = PackageFixtureTranscriptKind::PREDICATE_PROOF;
		const auto &proof = *node.Find("proof");
		ClosedMapping(proof, {"base", "restricted"}, path + ".proof");
		pages = ParsePages(Required(proof, "base", path + ".proof"), path + ".proof.base", true, limits, payloads);
		restricted_pages = ParsePages(Required(proof, "restricted", path + ".proof"), path + ".proof.restricted", true,
		                              limits, payloads);
	} else {
		transcript_kind = PackageFixtureTranscriptKind::AUTHORIZATION_RESOLUTION_FAILURE;
		const auto &failure = *node.Find("pre_request_failure");
		ClosedMapping(failure, {"checkpoint"}, path + ".pre_request_failure");
		if (Scalar(Required(failure, "checkpoint", path + ".pre_request_failure"),
		           path + ".pre_request_failure.checkpoint") != "authorization_resolution" ||
		    authentication != PackageFixtureAuthentication::BEARER_MISSING) {
			Fail(failure, path + ".pre_request_failure", "pre-request failure must be missing bearer resolution", id,
			     relation_id, operation_id);
		}
	}

	return {id,
	        relation_id,
	        operation_id,
	        std::move(parsed_covers),
	        std::move(parsed_inputs),
	        has_predicate,
	        std::move(predicate),
	        authentication,
	        transcript_kind,
	        std::move(pages),
	        std::move(restricted_pages),
	        ParseExpected(Required(node, "expected", path), path + ".expected", *relation)};
}

void AttachPages(std::vector<PackageFixturePage> &pages, const std::map<std::string, std::string> &payloads) {
	for (auto &page : pages) {
		const auto found = payloads.find(page.response.body_file);
		if (found == payloads.end()) {
			throw std::logic_error("verified fixture payload map is incomplete");
		}
		page.response.body = found->second;
	}
}

} // namespace

FixtureIndexFailure::FixtureIndexFailure(FixtureIndexFailureKind kind_p, PackageSourceCoordinate coordinate_p,
                                         std::string fixture_case_p, std::string relation_p, std::string operation_p,
                                         std::string safe_message_p)
    : kind(kind_p), coordinate(std::move(coordinate_p)), fixture_case(std::move(fixture_case_p)),
      relation(std::move(relation_p)), operation(std::move(operation_p)), safe_message(std::move(safe_message_p)) {
}

const char *FixtureIndexFailure::what() const noexcept {
	return safe_message.c_str();
}

FixtureIndexFailureKind FixtureIndexFailure::Kind() const noexcept {
	return kind;
}

const PackageSourceCoordinate &FixtureIndexFailure::Coordinate() const noexcept {
	return coordinate;
}

const std::string &FixtureIndexFailure::FixtureCase() const noexcept {
	return fixture_case;
}

const std::string &FixtureIndexFailure::Relation() const noexcept {
	return relation;
}

const std::string &FixtureIndexFailure::Operation() const noexcept {
	return operation;
}

ParsedPackageFixtureIndex ParsePackageFixtureIndex(const std::string &bytes,
                                                   const CompiledPackageGeneration &generation,
                                                   const PackageFixtureLimits &host_limits,
                                                   PackageCancellation &cancellation) {
	using namespace fixture_index_detail;
	const auto limits = EffectivePackageFixtureLimits(host_limits);
	FailsafeYamlBudget budget(FailsafeYamlLimits::V1());
	const auto root = ParseFailsafeYaml(INDEX_FILE, bytes, budget, cancellation);
	ClosedMapping(root, {"api_version", "kind", "package_digest", "cases"}, "$");
	if (Scalar(Required(root, "api_version", "$"), "$.api_version") != "cuac/v1" ||
	    Scalar(Required(root, "kind", "$"), "$.kind") != "fixture_index") {
		Fail(root, "$", "fixture index spec or kind is unsupported");
	}
	const auto digest = Scalar(Required(root, "package_digest", "$"), "$.package_digest");
	if (!IsDigest(digest) || digest != generation.Identity().PackageDigest()) {
		Fail(root, "$.package_digest", "fixture index is not bound to the compiled generation");
	}
	const auto &cases = Required(root, "cases", "$");
	RequireType(cases, FailsafeYamlNode::Kind::SEQUENCE, "$.cases");
	if (cases.Size() == 0 || cases.Size() > limits.max_cases) {
		Fail(cases, "$.cases", "fixture case budget is exhausted", "", "", "",
		     FixtureIndexFailureKind::RESOURCE_EXHAUSTED);
	}
	ParsedPackageFixtureIndex result;
	result.package_digest = digest;
	std::set<std::string> case_ids;
	for (std::size_t index = 0; index < cases.Size(); index++) {
		CheckCancellation(cancellation);
		auto parsed = ParseCase(cases.SequenceValue(index), index, generation, limits, result.payload_references);
		if (!case_ids.insert(parsed.id).second) {
			Fail(cases.SequenceValue(index), "$.cases[" + std::to_string(index) + "].id",
			     "fixture case identity is repeated", parsed.id, parsed.relation, parsed.operation);
		}
		result.case_coordinates.push_back(
		    Coordinate(cases.SequenceValue(index), "$.cases[" + std::to_string(index) + "]"));
		result.cases.push_back(std::move(parsed));
	}
	return result;
}

void AttachVerifiedFixturePayloads(ParsedPackageFixtureIndex &index,
                                   const std::map<std::string, std::string> &payloads) {
	for (auto &fixture_case : index.cases) {
		AttachPages(fixture_case.pages, payloads);
		AttachPages(fixture_case.restricted_pages, payloads);
	}
}

} // namespace internal
} // namespace connector
} // namespace cuac
