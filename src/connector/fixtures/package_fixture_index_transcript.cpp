#include "cuac/internal/connector/fixtures/package_fixture_index_parser.hpp"

#include <cstdlib>
#include <set>
#include <utility>

namespace cuac {
namespace connector {
namespace internal {
namespace fixture_index_detail {
namespace {

bool IsDigit(char value) {
	return value >= '0' && value <= '9';
}

PackageFixtureOrigin ParseOrigin(const FailsafeYamlNode &node, const std::string &path) {
	ClosedMapping(node, {"scheme", "host", "port"}, path);
	if (Scalar(Required(node, "scheme", path), path + ".scheme") != "https") {
		Fail(node, path, "fixture origin must use HTTPS");
	}
	const auto host = Scalar(Required(node, "host", path), path + ".host");
	const auto port_text = Scalar(Required(node, "port", path), path + ".port");
	if (host.empty() || port_text.empty() || port_text.size() > 5 || port_text.front() == '0') {
		Fail(node, path, "fixture origin is outside the closed host/port grammar");
	}
	for (const auto character : port_text) {
		if (!IsDigit(character)) {
			Fail(node, path, "fixture origin port is not canonical decimal");
		}
	}
	const auto port = std::strtoul(port_text.c_str(), nullptr, 10);
	if (port == 0 || port > 65535) {
		Fail(node, path, "fixture origin port is outside its range");
	}
	return {CompiledUrlScheme::HTTPS, host, static_cast<std::uint16_t>(port)};
}

std::vector<PackageFixtureHeader> ParseHeaders(const FailsafeYamlNode &node, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::SEQUENCE, path);
	if (node.Size() > 32) {
		Fail(node, path, "fixture response header budget is exhausted", "", "", "",
		     FixtureIndexFailureKind::RESOURCE_EXHAUSTED);
	}
	std::vector<PackageFixtureHeader> result;
	for (std::size_t index = 0; index < node.Size(); index++) {
		const auto &item = node.SequenceValue(index);
		const auto item_path = path + "[" + std::to_string(index) + "]";
		ClosedMapping(item, {"name", "value"}, item_path);
		result.push_back({Scalar(Required(item, "name", item_path), item_path + ".name"),
		                  Scalar(Required(item, "value", item_path), item_path + ".value")});
	}
	return result;
}

std::vector<PackageFixtureQueryField> ParseQuery(const FailsafeYamlNode &node, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::SEQUENCE, path);
	if (node.Size() > 64) {
		Fail(node, path, "fixture query-field budget is exhausted", "", "", "",
		     FixtureIndexFailureKind::RESOURCE_EXHAUSTED);
	}
	std::vector<PackageFixtureQueryField> result;
	for (std::size_t index = 0; index < node.Size(); index++) {
		const auto &item = node.SequenceValue(index);
		const auto item_path = path + "[" + std::to_string(index) + "]";
		ClosedMapping(item, {"name", "encoded_value"}, item_path);
		result.push_back({Scalar(Required(item, "name", item_path), item_path + ".name"),
		                  Scalar(Required(item, "encoded_value", item_path), item_path + ".encoded_value")});
	}
	return result;
}

std::vector<PackageFixtureGraphqlVariable> ParseVariables(const FailsafeYamlNode &node, const std::string &path) {
	RequireType(node, FailsafeYamlNode::Kind::SEQUENCE, path);
	if (node.Size() != 2) {
		Fail(node, path, "GraphQL fixture requires exactly two ordered variables");
	}
	std::vector<PackageFixtureGraphqlVariable> result;
	for (std::size_t index = 0; index < node.Size(); index++) {
		const auto &item = node.SequenceValue(index);
		const auto item_path = path + "[" + std::to_string(index) + "]";
		ClosedMapping(item, {"name", "type", "value"}, item_path);
		const auto type = Scalar(Required(item, "type", item_path), item_path + ".type");
		if (type != "INT_NON_NULL" && type != "STRING_NULLABLE") {
			Fail(item, item_path, "GraphQL fixture variable has an invalid type");
		}
		result.push_back({Scalar(Required(item, "name", item_path), item_path + ".name"),
		                  type == "INT_NON_NULL" ? CompiledGraphqlVariableType::INT_NON_NULL
		                                         : CompiledGraphqlVariableType::STRING_NULLABLE,
		                  ParseValue(Required(item, "value", item_path), item_path + ".value")});
	}
	return result;
}

PackageFixtureResponse ParseResponse(const FailsafeYamlNode &node, const std::string &path, bool proof,
                                     std::vector<std::pair<std::string, std::string>> &payloads) {
	ClosedMapping(node,
	              proof ? std::set<std::string> {"status", "headers", "body_file", "body_digest", "occurrence_ids"}
	                    : std::set<std::string> {"status", "headers", "body_file", "body_digest"},
	              path);
	const auto status_text = Scalar(Required(node, "status", path), path + ".status");
	if (status_text.size() != 3 || status_text.front() < '1' || status_text.front() > '5' || !IsDigit(status_text[1]) ||
	    !IsDigit(status_text[2])) {
		Fail(node, path, "fixture response status is invalid");
	}
	const auto body_file = Scalar(Required(node, "body_file", path), path + ".body_file");
	const auto body_digest = Scalar(Required(node, "body_digest", path), path + ".body_digest");
	if (!IsBodyFile(body_file) || !IsDigest(body_digest)) {
		Fail(node, path, "fixture response body identity is invalid");
	}
	payloads.emplace_back(body_file, body_digest);
	std::vector<std::string> occurrences;
	if (proof) {
		const auto &source = Required(node, "occurrence_ids", path);
		RequireType(source, FailsafeYamlNode::Kind::SEQUENCE, path + ".occurrence_ids");
		std::set<std::string> unique;
		for (std::size_t index = 0; index < source.Size(); index++) {
			const auto value =
			    Scalar(source.SequenceValue(index), path + ".occurrence_ids[" + std::to_string(index) + "]");
			if (!IsId(value) || !unique.insert(value).second) {
				Fail(source.SequenceValue(index), path + ".occurrence_ids[" + std::to_string(index) + "]",
				     "proof occurrence identity is invalid or repeated");
			}
			occurrences.push_back(value);
		}
	}
	return {static_cast<std::uint16_t>(std::strtoul(status_text.c_str(), nullptr, 10)),
	        ParseHeaders(Required(node, "headers", path), path + ".headers"),
	        body_file,
	        body_digest,
	        "",
	        std::move(occurrences)};
}

} // namespace

PackageFixtureRequest ParseRequest(const FailsafeYamlNode &node, const std::string &path) {
	const auto protocol = Scalar(Required(node, "protocol", path), path + ".protocol");
	if (protocol == "rest") {
		ClosedMapping(node, {"protocol", "method", "origin", "path", "query", "authorization"}, path);
		if (Scalar(Required(node, "method", path), path + ".method") != "GET") {
			Fail(node, path, "REST fixture method must be GET");
		}
		const auto authorization = Scalar(Required(node, "authorization", path), path + ".authorization");
		if (authorization != "none" && authorization != "bearer_capability") {
			Fail(node, path, "REST fixture authorization is outside the closed vocabulary");
		}
		return {CompiledProtocol::REST,
		        ParseOrigin(Required(node, "origin", path), path + ".origin"),
		        Scalar(Required(node, "path", path), path + ".path"),
		        ParseQuery(Required(node, "query", path), path + ".query"),
		        "",
		        {},
		        "",
		        authorization == "bearer_capability"};
	}
	if (protocol != "graphql") {
		Fail(node, path, "fixture request protocol is outside the closed vocabulary");
	}
	ClosedMapping(
	    node,
	    {"protocol", "method", "endpoint", "document_digest", "variables", "serialized_body_digest", "authorization"},
	    path);
	if (Scalar(Required(node, "method", path), path + ".method") != "POST" ||
	    Scalar(Required(node, "authorization", path), path + ".authorization") != "bearer_capability") {
		Fail(node, path, "GraphQL fixture method or authorization is invalid");
	}
	const auto &endpoint = Required(node, "endpoint", path);
	ClosedMapping(endpoint, {"origin", "path"}, path + ".endpoint");
	const auto document_digest = Scalar(Required(node, "document_digest", path), path + ".document_digest");
	const auto body_digest = Scalar(Required(node, "serialized_body_digest", path), path + ".serialized_body_digest");
	if (!IsDigest(document_digest) || !IsDigest(body_digest)) {
		Fail(node, path, "GraphQL fixture digest is invalid");
	}
	return {CompiledProtocol::GRAPHQL,
	        ParseOrigin(Required(endpoint, "origin", path + ".endpoint"), path + ".endpoint.origin"),
	        Scalar(Required(endpoint, "path", path + ".endpoint"), path + ".endpoint.path"),
	        {},
	        document_digest,
	        ParseVariables(Required(node, "variables", path), path + ".variables"),
	        body_digest,
	        true};
}

std::vector<PackageFixturePage> ParsePages(const FailsafeYamlNode &run, const std::string &path, bool proof,
                                           const PackageFixtureLimits &limits,
                                           std::vector<std::pair<std::string, std::string>> &payloads) {
	ClosedMapping(run, {"pages"}, path);
	const auto &pages = Required(run, "pages", path);
	RequireType(pages, FailsafeYamlNode::Kind::SEQUENCE, path + ".pages");
	if (pages.Size() == 0 || pages.Size() > limits.max_pages_per_case) {
		Fail(pages, path + ".pages", "fixture response-page budget is exhausted", "", "", "",
		     FixtureIndexFailureKind::RESOURCE_EXHAUSTED);
	}
	std::vector<PackageFixturePage> result;
	for (std::size_t index = 0; index < pages.Size(); index++) {
		const auto &page = pages.SequenceValue(index);
		const auto page_path = path + ".pages[" + std::to_string(index) + "]";
		ClosedMapping(page, {"request", "response"}, page_path);
		auto request = ParseRequest(Required(page, "request", page_path), page_path + ".request");
		if (proof && request.protocol != CompiledProtocol::REST) {
			Fail(page, page_path, "predicate proof pages must use REST");
		}
		result.push_back({std::move(request), ParseResponse(Required(page, "response", page_path),
		                                                    page_path + ".response", proof, payloads)});
	}
	return result;
}

} // namespace fixture_index_detail
} // namespace internal
} // namespace connector
} // namespace cuac
