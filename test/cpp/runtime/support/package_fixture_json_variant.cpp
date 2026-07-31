#include "runtime/support/package_fixture_json_variant_internal.hpp"

#include "cuac/internal/runtime/decoding/strict_json_reader.hpp"
#include "cuac/internal/runtime/admission/graphql_plan_admission.hpp"
#include "cuac/internal/runtime/admission/http_plan_admission.hpp"
#include "cuac/internal/runtime/executor/http_scan_executor.hpp"

#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cuac_test {
namespace internal {
namespace {

using Reader = cuac::internal::StrictJsonReader;

struct JsonSpan {
	std::size_t begin;
	std::size_t end;
	std::size_t erase_begin;
	std::size_t erase_end;
};

struct CompactJsonNode {
	std::string name;
	std::string value;
	bool has_value;
	std::vector<CompactJsonNode> children;
};

void CheckCancellation(cuac::ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
}

uint64_t AddChecked(uint64_t left, uint64_t right) {
	if (right > std::numeric_limits<uint64_t>::max() - left) {
		throw std::invalid_argument("closed Runtime fixture derived body size overflowed");
	}
	return left + right;
}

uint64_t MultiplyChecked(uint64_t left, uint64_t right) {
	if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
		throw std::invalid_argument("closed Runtime fixture derived body size overflowed");
	}
	return left * right;
}

cuac::internal::HttpExecutionProfile PublicFixtureProfile() {
	return {cuac::PlannedUrlScheme::HTTPS,
	        "",
	        0,
	        false,
	        false,
	        false,
	        cuac::PAGINATION_MAX_EXECUTION_MILLISECONDS,
	        cuac::PAGINATION_MAX_DECODED_RECORDS_PER_PAGE,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_STEP,
	        cuac::RETRY_MAX_REQUEST_ATTEMPTS_PER_SCAN,
	        cuac::RETRY_MAX_DELAY_MILLISECONDS,
	        cuac::RETRY_MAX_CUMULATIVE_WAITING_MILLISECONDS_PER_SCAN};
}

bool FindObjectPath(Reader &reader, const std::vector<std::string> &path, std::size_t path_index, JsonSpan &span,
                    std::size_t *matched_segments = nullptr) {
	reader.SkipWhitespace();
	if (reader.Peek() != '{') {
		reader.SkipValue();
		return false;
	}
	reader.Expect('{');
	reader.SkipWhitespace();
	std::size_t previous_separator = std::numeric_limits<std::size_t>::max();
	bool found = false;
	bool selected_member_seen = false;
	while (reader.Peek() != '}') {
		const auto member_begin = reader.Position();
		reader.RequireObjectKey();
		const auto key = reader.ParseObjectKey();
		reader.SkipWhitespace();
		reader.Expect(':');
		reader.SkipWhitespace();
		const auto value_begin = reader.Position();
		if (key.Equals(path[path_index])) {
			if (selected_member_seen) {
				throw std::invalid_argument("closed Runtime fixture base response has an ambiguous selected path");
			}
			selected_member_seen = true;
			if (path_index + 1 < path.size()) {
				if (reader.Peek() == '{') {
					JsonSpan nested;
					if (FindObjectPath(reader, path, path_index + 1, nested, matched_segments)) {
						span = nested;
						found = true;
					}
				} else if (matched_segments) {
					reader.SkipValue();
					span = {value_begin, reader.Position(), member_begin, reader.Position()};
					*matched_segments = path_index + 1;
					found = true;
				} else {
					reader.SkipValue();
				}
			} else {
				reader.SkipValue();
				const auto value_end = reader.Position();
				reader.SkipWhitespace();
				const auto after_value = reader.Position();
				JsonSpan candidate {value_begin, value_end, member_begin, after_value};
				if (reader.Peek() == ',') {
					candidate.erase_end = after_value + 1;
				} else if (previous_separator != std::numeric_limits<std::size_t>::max()) {
					candidate.erase_begin = previous_separator;
				}
				span = candidate;
				if (matched_segments) {
					*matched_segments = path.size();
				}
				found = true;
			}
		} else {
			reader.SkipValue();
		}
		reader.SkipWhitespace();
		if (reader.Peek() == ',') {
			previous_separator = reader.Position();
			reader.ObjectSeparator();
		} else if (reader.Peek() != '}') {
			throw std::invalid_argument("controlled fixture JSON object has an invalid separator");
		}
	}
	reader.Expect('}');
	return found;
}

bool FindPath(const std::vector<std::string> &path, Reader &reader, JsonSpan &span) {
	reader.Reset();
	reader.SkipWhitespace();
	if (path.empty()) {
		span.begin = reader.Position();
		reader.SkipValue();
		span.end = reader.Position();
		span.erase_begin = span.begin;
		span.erase_end = span.end;
		return true;
	}
	return FindObjectPath(reader, path, 0, span);
}

bool FindFirstArrayElement(Reader &reader, const JsonSpan &array, JsonSpan &element) {
	reader.SetPosition(array.begin);
	reader.SkipWhitespace();
	if (reader.Peek() != '[') {
		return false;
	}
	reader.Expect('[');
	reader.SkipWhitespace();
	if (reader.Peek() == ']') {
		return false;
	}
	element.begin = reader.Position();
	reader.SkipValue();
	element.end = reader.Position();
	element.erase_begin = element.begin;
	element.erase_end = element.end;
	return true;
}

std::string ReplaceSpan(const std::string &body, const JsonSpan &span, const std::string &replacement,
                        bool remove_member) {
	const auto begin = remove_member ? span.erase_begin : span.begin;
	const auto end = remove_member ? span.erase_end : span.end;
	if (begin > end || end > body.size()) {
		throw std::logic_error("controlled fixture JSON mutation produced an invalid span");
	}
	std::string result;
	result.reserve(body.size() - (end - begin) + replacement.size());
	result.append(body, 0, begin);
	result += replacement;
	result.append(body, end, body.size() - end);
	return result;
}

Reader MakeReader(const std::string &body, cuac::ExecutionControl &control) {
	return Reader(body, cuac::PAGINATION_MAX_JSON_NESTING, std::chrono::steady_clock::now() + std::chrono::seconds(5),
	              control);
}

CompactJsonNode *FindOrAddChild(CompactJsonNode &parent, const std::string &name) {
	for (auto &child : parent.children) {
		if (child.name == name) {
			return &child;
		}
	}
	parent.children.push_back({name, "", false, {}});
	return &parent.children.back();
}

void InsertCompactValue(CompactJsonNode &root, const std::vector<std::string> &path, std::size_t segments,
                        const std::string &value) {
	CompactJsonNode *node = &root;
	for (std::size_t index = 0; index < segments; index++) {
		if (node->has_value) {
			if (node->value == "null") {
				return;
			}
			throw std::invalid_argument("planned response paths conflict while building a compact fixture record");
		}
		node = FindOrAddChild(*node, path[index]);
	}
	if (!node->children.empty() || (node->has_value && node->value != value)) {
		throw std::invalid_argument("planned response paths conflict while building a compact fixture record");
	}
	node->value = value;
	node->has_value = true;
}

void AppendBounded(std::string &result, const std::string &value, uint64_t maximum, cuac::ExecutionControl &control) {
	CheckCancellation(control);
	if (static_cast<uint64_t>(result.size()) > maximum ||
	    static_cast<uint64_t>(value.size()) > maximum - result.size()) {
		throw std::invalid_argument("compact planned fixture record exceeds its derived body limit");
	}
	result += value;
}

std::string RenderJsonObjectKey(const std::string &key) {
	static const char hex[] = "0123456789abcdef";
	std::string result = "\"";
	for (const auto byte : key) {
		const auto value = static_cast<unsigned char>(byte);
		switch (value) {
		case '"':
			result += "\\\"";
			break;
		case '\\':
			result += "\\\\";
			break;
		case '\b':
			result += "\\b";
			break;
		case '\f':
			result += "\\f";
			break;
		case '\n':
			result += "\\n";
			break;
		case '\r':
			result += "\\r";
			break;
		case '\t':
			result += "\\t";
			break;
		default:
			if (value < 0x20) {
				result += "\\u00";
				result.push_back(hex[value >> 4]);
				result.push_back(hex[value & 0x0f]);
			} else {
				result.push_back(byte);
			}
		}
	}
	return result + "\":";
}

void RenderCompactNode(const CompactJsonNode &node, std::string &result, uint64_t maximum,
                       cuac::ExecutionControl &control) {
	if (node.has_value) {
		AppendBounded(result, node.value, maximum, control);
		return;
	}
	AppendBounded(result, "{", maximum, control);
	for (std::size_t index = 0; index < node.children.size(); index++) {
		if (index != 0) {
			AppendBounded(result, ",", maximum, control);
		}
		AppendBounded(result, RenderJsonObjectKey(node.children[index].name), maximum, control);
		RenderCompactNode(node.children[index], result, maximum, control);
	}
	AppendBounded(result, "}", maximum, control);
}

std::string BuildCompactRuntimeFixtureRecord(const std::string &body, const RuntimeFixtureJsonShape &shape,
                                             const JsonSpan &record, uint64_t maximum,
                                             cuac::ExecutionControl &control) {
	CompactJsonNode root {"", "", false, {}};
	auto reader = MakeReader(body, control);
	reader.ValidateDocument();
	for (const auto &column : shape.columns) {
		CheckCancellation(control);
		reader.SetPosition(record.begin);
		JsonSpan value;
		std::size_t matched_segments = 0;
		if (!FindObjectPath(reader, column.path, 0, value, &matched_segments) || matched_segments == 0) {
			throw std::invalid_argument("closed Runtime fixture first record is missing a planned column path");
		}
		const auto raw_value = body.substr(value.begin, value.end - value.begin);
		if (matched_segments != column.path.size() && (!column.nullable || raw_value != "null")) {
			throw std::invalid_argument("closed Runtime fixture planned column path ends at a non-null scalar");
		}
		InsertCompactValue(root, column.path, matched_segments, raw_value);
	}
	std::string result;
	RenderCompactNode(root, result, maximum, control);
	return result;
}

} // namespace

RuntimeFixtureJsonShape AdmitRuntimeFixtureJsonShape(const cuac::ScanPlan &plan) {
	RuntimeFixtureJsonShape result;
	const auto profile = PublicFixtureProfile();
	if (plan.Operation().Protocol() == cuac::PlannedProtocol::GRAPHQL) {
		auto admitted = cuac::internal::TryAdmitGraphqlPlan(plan, profile);
		if (!admitted) {
			throw std::invalid_argument("closed Runtime fixture GraphQL plan was not admitted");
		}
		result.records_are_array = true;
		result.records_path = admitted->NodesPath();
		for (const auto &column : admitted->Columns()) {
			result.columns.push_back({column.name, column.type, column.nullable, column.response_path});
		}
		return result;
	}

	if (plan.Operation().Protocol() != cuac::PlannedProtocol::REST) {
		throw std::invalid_argument("closed Runtime fixture plan has an unknown protocol");
	}
	if (plan.Pagination().Strategy() == cuac::PlannedPaginationStrategy::DISABLED) {
		auto admitted = cuac::internal::TryAdmitSingleResponseHttpPlan(plan, profile);
		if (!admitted) {
			throw std::invalid_argument("closed Runtime fixture REST plan was not admitted");
		}
		result.records_are_array = admitted->ResponseSource() != cuac::PlannedResponseSource::ROOT_OBJECT;
		result.records_path = admitted->RecordsPath();
		for (const auto &column : admitted->Columns()) {
			result.columns.push_back({column.name, column.type, column.nullable, column.source_path});
		}
		return result;
	}
	auto admitted = cuac::internal::TryAdmitPaginatedRestPlan(plan, profile);
	if (!admitted) {
		throw std::invalid_argument("closed Runtime fixture paginated REST plan was not admitted");
	}
	result.records_are_array = true;
	result.records_path = admitted->RecordsPath();
	for (const auto &column : admitted->Columns()) {
		result.columns.push_back({column.name, column.type, column.nullable, column.source_path});
	}
	return result;
}

std::string ReplaceFirstRuntimeFixtureColumn(const std::string &body, const RuntimeFixtureJsonShape &shape,
                                             std::size_t column_ordinal, const std::string &replacement,
                                             bool remove_member, cuac::ExecutionControl &control) {
	if (column_ordinal >= shape.columns.size()) {
		throw std::invalid_argument("closed Runtime fixture column ordinal is outside the admitted schema");
	}
	auto reader = MakeReader(body, control);
	reader.ValidateDocument();
	JsonSpan record;
	if (shape.records_are_array) {
		JsonSpan records;
		if (!FindPath(shape.records_path, reader, records) || !FindFirstArrayElement(reader, records, record)) {
			throw std::invalid_argument("closed Runtime fixture base response has no first planned record");
		}
	} else {
		if (!FindPath(shape.records_path, reader, record)) {
			throw std::invalid_argument("closed Runtime fixture base response is missing its planned root record");
		}
	}
	reader.SetPosition(record.begin);
	JsonSpan column;
	if (!FindObjectPath(reader, shape.columns[column_ordinal].path, 0, column)) {
		throw std::invalid_argument("closed Runtime fixture first record is missing the planned column path");
	}
	return ReplaceSpan(body, column, replacement, remove_member);
}

std::string RepeatFirstRuntimeFixtureRecord(const std::string &body, const RuntimeFixtureJsonShape &shape,
                                            uint64_t record_count, uint64_t max_derived_body_bytes,
                                            cuac::ExecutionControl &control) {
	if (!shape.records_are_array) {
		throw std::invalid_argument("closed Runtime fixture record repetition requires a collection response");
	}
	auto reader = MakeReader(body, control);
	reader.ValidateDocument();
	JsonSpan records;
	if (!FindPath(shape.records_path, reader, records)) {
		throw std::invalid_argument("closed Runtime fixture base response is missing its planned record collection");
	}
	std::string value;
	if (record_count != 0) {
		JsonSpan record;
		if (!FindFirstArrayElement(reader, records, record)) {
			throw std::invalid_argument("closed Runtime fixture base response has no first planned record");
		}
		value = BuildCompactRuntimeFixtureRecord(body, shape, record, max_derived_body_bytes, control);
	}
	const auto fixed_bytes = AddChecked(static_cast<uint64_t>(body.size() - (records.end - records.begin)), 2);
	auto derived_bytes = AddChecked(fixed_bytes, MultiplyChecked(record_count, static_cast<uint64_t>(value.size())));
	if (record_count != 0) {
		derived_bytes = AddChecked(derived_bytes, record_count - 1);
	}
	if (max_derived_body_bytes == 0 || derived_bytes > max_derived_body_bytes ||
	    derived_bytes > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw std::invalid_argument("compact planned fixture response exceeds a competing body limit");
	}
	std::string replacement = "[";
	replacement.reserve(static_cast<std::size_t>(derived_bytes - fixed_bytes + 2));
	for (uint64_t index = 0; index < record_count; index++) {
		CheckCancellation(control);
		if (index != 0) {
			replacement.push_back(',');
		}
		replacement += value;
	}
	replacement.push_back(']');
	return ReplaceSpan(body, records, replacement, false);
}

std::string ReplaceRuntimeFixturePath(const std::string &body, const std::vector<std::string> &path,
                                      const std::string &replacement, bool remove_member,
                                      cuac::ExecutionControl &control) {
	if (path.empty()) {
		throw std::invalid_argument("closed Runtime fixture path mutation requires a nonempty object path");
	}
	auto reader = MakeReader(body, control);
	reader.ValidateDocument();
	JsonSpan span;
	if (!FindPath(path, reader, span)) {
		throw std::invalid_argument("closed Runtime fixture base response is missing the selected path");
	}
	return ReplaceSpan(body, span, replacement, remove_member);
}

} // namespace internal
} // namespace cuac_test
