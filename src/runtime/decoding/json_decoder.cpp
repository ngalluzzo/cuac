#include "cuac/internal/runtime/decoding/json_decoder.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <set>
#include <utility>

namespace cuac {
namespace internal {
namespace {

// This module deliberately keeps syntax validation, cursor movement, selected
// field extraction, lossless conversion, cancellation, and retained-memory
// accounting in one parser. Those responsibilities share one traversal and
// one budget owner; splitting them would either rescan input or risk accepting
// malformed JSON outside the selected record path.

[[noreturn]] void MalformedJson() {
	throw ExecutionError(ErrorStage::DECODE, "", "HTTP response is not valid JSON");
}

void Checkpoint(ExecutionControl &control, std::chrono::steady_clock::time_point deadline) {
	if (control.IsCancellationRequested()) {
		throw ExecutionCancelled();
	}
	if (std::chrono::steady_clock::now() >= deadline) {
		throw ExecutionError(ErrorStage::RESOURCE, "wall_milliseconds", "execution exceeded its wall-time budget");
	}
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t &result) noexcept {
	if (right > std::numeric_limits<uint64_t>::max() - left) {
		return false;
	}
	result = left + right;
	return true;
}

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t &result) noexcept {
	if (left != 0 && right > std::numeric_limits<uint64_t>::max() / left) {
		return false;
	}
	result = left * right;
	return true;
}

void RequireMemory(uint64_t current, uint64_t addition, uint64_t limit, uint64_t &updated) {
	if (!CheckedAdd(current, addition, updated) || updated > limit) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes", "decoded rows exceeded their memory budget");
	}
}

bool IsPathPrefix(const std::vector<std::string> &prefix, const std::vector<std::string> &path) {
	return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
}

bool IsValidOutputType(const OutputValueType &type) {
	switch (type.element_kind) {
	case ValueKind::BIGINT:
	case ValueKind::VARCHAR:
	case ValueKind::BOOLEAN:
	case ValueKind::DOUBLE:
		break;
	default:
		return false;
	}
	return (type.shape == ValueShape::SCALAR && !type.element_nullable) || type.shape == ValueShape::ARRAY;
}

void ValidatePlan(const JsonDecodePlan &plan) {
	const bool has_records_path = !plan.records_path.empty();
	const bool source_valid =
	    (plan.response_source == JsonResponseSource::JSON_PATH_MANY && has_records_path) ||
	    (plan.response_source == JsonResponseSource::ROOT_ARRAY && plan.records_path.empty()) ||
	    (plan.response_source == JsonResponseSource::ROOT_OBJECT && plan.records_path.empty() && plan.max_records == 1);
	if (!source_valid || plan.columns.empty() || plan.max_records == 0 || plan.max_string_bytes == 0 ||
	    plan.max_json_nesting == 0 || plan.max_decoded_memory_bytes == 0 ||
	    plan.max_records > static_cast<uint64_t>(std::numeric_limits<std::size_t>::max())) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid schema or budget");
	}
	for (const auto &segment : plan.records_path) {
		if (segment.empty()) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid schema or budget");
		}
	}
	for (std::size_t index = 0; index < plan.columns.size(); index++) {
		if (plan.columns[index].output_name.empty() || plan.columns[index].json_path.empty() ||
		    !IsValidOutputType(plan.columns[index].type)) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid schema or budget");
		}
		for (const auto &segment : plan.columns[index].json_path) {
			if (segment.empty()) {
				throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid schema or budget");
			}
		}
		for (std::size_t other = 0; other < index; other++) {
			if (plan.columns[index].output_name == plan.columns[other].output_name ||
			    plan.columns[index].json_path == plan.columns[other].json_path ||
			    IsPathPrefix(plan.columns[index].json_path, plan.columns[other].json_path) ||
			    IsPathPrefix(plan.columns[other].json_path, plan.columns[index].json_path)) {
				throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid schema or budget");
			}
		}
	}
}

// This parser validates the entire document before extracting the declared
// array. Ignored values therefore cannot hide malformed JSON. Retained-memory
// accounting includes output vector storage, typed values, and owned string
// capacity; transient parser strings remain bounded by the response budget.
class JsonParser {
public:
	JsonParser(const std::string &input_p, const JsonDecodePlan &plan_p, ExecutionControl &control_p)
	    : input(input_p), plan(plan_p), control(control_p), position(0), decoded_memory(0), pending_string_memory(0),
	      pending_array_memory(0), pending_structural_memory(0), continuation_memory(0), peak_memory(0) {
	}

	std::vector<TypedRow> ParseResponse() {
		ValidateDocument();
		SkipWhitespace();
		const bool root_matches =
		    plan.response_source == JsonResponseSource::ROOT_ARRAY ? Peek() == '[' : Peek() == '{';
		if (!root_matches) {
			const auto field = !plan.records_path.empty() ? plan.records_path.back() : std::string();
			throw ExecutionError(ErrorStage::SCHEMA,
			                     plan.response_source == JsonResponseSource::JSON_PATH_MANY ? field : "",
			                     "response root does not match the declared schema");
		}
		if (plan.response_source == JsonResponseSource::ROOT_OBJECT) {
			return ParseRootObject();
		}
		if (plan.response_source == JsonResponseSource::ROOT_ARRAY) {
			return ParseRootArray();
		}
		return ParseRecordArrayResponse();
	}

	std::string TakeNextUrl() {
		return std::move(next_url);
	}

	uint64_t RetainedMemoryBytes() const noexcept {
		return decoded_memory;
	}

	uint64_t PeakMemoryBytes() const noexcept {
		return peak_memory;
	}

	uint64_t ContinuationMemoryBytes() const noexcept {
		return continuation_memory;
	}

	void ReconcileTakenContinuationMemory(uint64_t actual) {
		if (continuation_memory == 0 && actual != 0) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "continuation memory accounting failed");
		}
		if (actual > continuation_memory) {
			uint64_t current = 0;
			if (!CheckedAdd(decoded_memory, pending_string_memory, current) ||
			    !CheckedAdd(current, pending_array_memory, current) ||
			    !CheckedAdd(current, pending_structural_memory, current) || !CheckedAdd(current, actual, current) ||
			    current > plan.max_decoded_memory_bytes) {
				throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
				                     "response continuation exceeded its memory budget");
			}
		}
		continuation_memory = actual;
		ObserveCurrentMemory();
	}

private:
	struct ParsedSlot {
		ParsedSlot() : bigint_value(0), boolean_value(false), double_value(0.0), seen(false), valid(true) {
		}

		int64_t bigint_value;
		std::string varchar_value;
		bool boolean_value;
		double double_value;
		bool seen;
		bool valid;
		std::vector<TypedScalarValue> elements;
	};

	std::vector<TypedRow> AllocateResult() {
		std::vector<TypedRow> result;
		uint64_t requested_storage = 0;
		if (!CheckedMultiply(plan.max_records, static_cast<uint64_t>(sizeof(TypedRow)), requested_storage) ||
		    requested_storage > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		try {
			result.reserve(static_cast<std::size_t>(plan.max_records));
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows could not be allocated within their memory budget");
		}
		uint64_t row_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(result.capacity()), static_cast<uint64_t>(sizeof(TypedRow)),
		                     row_storage) ||
		    row_storage > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		decoded_memory = row_storage;
		ObserveCurrentMemory();
		return result;
	}

	void ObserveCurrentMemory() {
		uint64_t current = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, current) ||
		    !CheckedAdd(current, pending_array_memory, current) ||
		    !CheckedAdd(current, pending_structural_memory, current) ||
		    !CheckedAdd(current, continuation_memory, current) || current > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		peak_memory = std::max(peak_memory, current);
	}

	void ReservePendingStringMemory(uint64_t addition) {
		uint64_t retained = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || !CheckedAdd(retained, addition, retained) ||
		    retained > plan.max_decoded_memory_bytes ||
		    !CheckedAdd(pending_string_memory, addition, pending_string_memory)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded string exceeded its memory budget");
		}
		ObserveCurrentMemory();
	}

	void ReserveStructuralMemory(uint64_t addition) {
		uint64_t retained = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || !CheckedAdd(retained, addition, retained) ||
		    retained > plan.max_decoded_memory_bytes ||
		    !CheckedAdd(pending_structural_memory, addition, pending_structural_memory)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded row staging exceeded its memory budget");
		}
		ObserveCurrentMemory();
	}

	void ReconcileStructuralMemory(uint64_t reserved, uint64_t actual) {
		if (actual > reserved) {
			ReserveStructuralMemory(actual - reserved);
		} else {
			pending_structural_memory -= reserved - actual;
		}
	}

	std::vector<TypedRow> ParseRootObject() {
		auto result = AllocateResult();
		result.push_back(ParseRecord());
		SkipWhitespace();
		if (position != input.size()) {
			MalformedJson();
		}
		return result;
	}

	std::vector<TypedRow> ParseRootArray() {
		auto result = AllocateResult();
		ParseRecords(result);
		SkipWhitespace();
		if (position != input.size()) {
			MalformedJson();
		}
		return result;
	}

	std::vector<TypedRow> ParseRecordArrayResponse() {
		auto result = AllocateResult();
		const auto path = EffectiveRecordsPath();
		ParseRecordArrayObject(path, 0, result);
		SkipWhitespace();
		if (position != input.size()) {
			MalformedJson();
		}
		// Extract the optional page-level continuation scalar declared by
		// response_next pagination. The document is already validated; this
		// focused walk follows the continuation path segments from the root
		// and extracts the leaf scalar (string, null, or absent). A non-string
		// non-null leaf is the next_field_wrong_type_rejected case.
		if (!plan.page_continuation_path.empty()) {
			position = 0;
			ExtractContinuation(plan.page_continuation_path, 0);
		}
		return result;
	}

	void ExtractContinuation(const std::vector<std::string> &cont_path, std::size_t cont_depth) {
		SkipWhitespace();
		if (Peek() != '{') {
			return;
		}
		Expect('{');
		SkipWhitespace();
		bool found = false;
		while (Peek() != '}') {
			RequireObjectKey();
			const auto key = ParseString();
			SkipWhitespace();
			Expect(':');
			SkipWhitespace();
			if (key == cont_path[cont_depth]) {
				if (found) {
					throw ExecutionError(ErrorStage::SCHEMA, cont_path.back(),
					                     "response_next continuation field is duplicated");
				}
				found = true;
				if (cont_depth + 1 == cont_path.size()) {
					ExtractContinuationScalar(cont_path);
				} else if (Peek() == '{') {
					ExtractContinuation(cont_path, cont_depth + 1);
				} else {
					// Intermediate path segment is not an object: the
					// continuation path is absent (no next page).
					SkipValue();
				}
			} else {
				SkipValue();
			}
			ConsumeObjectSeparator();
		}
		Expect('}');
	}

	void ExtractContinuationScalar(const std::vector<std::string> &cont_path) {
		if (Peek() == '"') {
			const JsonColumnPlan continuation_column("extracted_string_bytes", "continuation", ValueKind::VARCHAR);
			auto value = ParseString(&continuation_column);
			const auto charged_capacity = static_cast<uint64_t>(value.capacity());
			next_url.swap(value);
			const auto retained_capacity = static_cast<uint64_t>(next_url.capacity());
			if (retained_capacity > charged_capacity) {
				ReservePendingStringMemory(retained_capacity - charged_capacity);
			} else {
				pending_string_memory -= charged_capacity - retained_capacity;
			}
			continuation_memory = retained_capacity;
			pending_string_memory -= retained_capacity;
			ObserveCurrentMemory();
			return;
		}
		if (Peek() == 'n') {
			ParseLiteral("null");
			return;
		}
		// A present non-string non-null value at the continuation path is the
		// next_field_wrong_type_rejected case (RFC 0016).
		SkipValue();
		throw ExecutionError(ErrorStage::SCHEMA, cont_path.back(),
		                     "response_next continuation field has an incompatible type");
	}

	const std::vector<std::string> &EffectiveRecordsPath() const {
		return plan.records_path;
	}

	const std::string &RecordsDiagnosticField(const std::vector<std::string> &path) const {
		return path.back();
	}

	void ParseRecordArrayObject(const std::vector<std::string> &path, std::size_t depth,
	                            std::vector<TypedRow> &result) {
		Expect('{');
		SkipWhitespace();
		bool found_records = false;
		while (Peek() != '}') {
			RequireObjectKey();
			const auto key = ParseString();
			SkipWhitespace();
			Expect(':');
			SkipWhitespace();
			if (key == path[depth]) {
				if (found_records) {
					throw ExecutionError(ErrorStage::SCHEMA, RecordsDiagnosticField(path),
					                     "required response field is duplicated");
				}
				found_records = true;
				if (depth + 1 == path.size()) {
					if (Peek() != '[') {
						SkipValue();
						throw ExecutionError(ErrorStage::SCHEMA, RecordsDiagnosticField(path),
						                     "required response field has an incompatible type");
					}
					ParseRecords(result);
				} else if (Peek() != '{') {
					SkipValue();
					throw ExecutionError(ErrorStage::SCHEMA, RecordsDiagnosticField(path),
					                     "required response field has an incompatible type");
				} else {
					ParseRecordArrayObject(path, depth + 1, result);
				}
			} else {
				SkipValue();
			}
			ConsumeObjectSeparator();
		}
		Expect('}');
		if (!found_records) {
			throw ExecutionError(ErrorStage::SCHEMA, RecordsDiagnosticField(path),
			                     "required response field is missing");
		}
	}

	void ValidateDocument() {
		SkipWhitespace();
		SkipValue();
		SkipWhitespace();
		if (position != input.size()) {
			MalformedJson();
		}
		position = 0;
	}

	void Check() const {
		Checkpoint(control, plan.deadline);
	}

	void SkipWhitespace() {
		while (position < input.size()) {
			Check();
			const auto character = input[position];
			if (character != ' ' && character != '\n' && character != '\r' && character != '\t') {
				break;
			}
			position++;
		}
	}

	char Peek() const noexcept {
		return position < input.size() ? input[position] : '\0';
	}

	void Expect(char expected) {
		Check();
		if (position >= input.size() || input[position] != expected) {
			MalformedJson();
		}
		position++;
	}

	void RequireObjectKey() const {
		if (Peek() != '"') {
			MalformedJson();
		}
	}

	void ConsumeObjectSeparator() {
		SkipWhitespace();
		if (Peek() == ',') {
			position++;
			SkipWhitespace();
			if (Peek() == '}') {
				MalformedJson();
			}
			return;
		}
		if (Peek() != '}') {
			MalformedJson();
		}
	}

	void ConsumeArraySeparator() {
		SkipWhitespace();
		if (Peek() == ',') {
			position++;
			SkipWhitespace();
			if (Peek() == ']') {
				MalformedJson();
			}
			return;
		}
		if (Peek() != ']') {
			MalformedJson();
		}
	}

	void ReserveRetainedString(std::string &result, std::size_t addition, const JsonColumnPlan *column,
	                           uint64_t &charged_capacity) {
		if (column == nullptr) {
			return;
		}
		if (addition > plan.max_string_bytes || result.size() > plan.max_string_bytes - addition) {
			throw ExecutionError(ErrorStage::RESOURCE, column->output_name,
			                     "required response string exceeded its byte budget");
		}
		const auto required = result.size() + addition;
		if (required <= result.capacity() && charged_capacity == static_cast<uint64_t>(result.capacity())) {
			return;
		}
		const auto requested = static_cast<uint64_t>(std::max(required, result.capacity()));
		uint64_t retained = 0;
		if (requested < charged_capacity || !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || !CheckedAdd(retained, requested, retained) ||
		    retained > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded string exceeded its memory budget");
		}
		peak_memory = std::max(peak_memory, retained);
		try {
			if (required > result.capacity()) {
				result.reserve(required);
			}
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded string could not be allocated within its memory budget");
		}
		const auto actual = static_cast<uint64_t>(result.capacity());
		uint64_t co_live = 0;
		if (actual < charged_capacity || !CheckedAdd(decoded_memory, pending_string_memory, co_live) ||
		    !CheckedAdd(co_live, pending_array_memory, co_live) ||
		    !CheckedAdd(co_live, pending_structural_memory, co_live) ||
		    !CheckedAdd(co_live, continuation_memory, co_live) || !CheckedAdd(co_live, actual, co_live) ||
		    co_live > plan.max_decoded_memory_bytes ||
		    !CheckedAdd(pending_string_memory, actual - charged_capacity, pending_string_memory) ||
		    !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || retained > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded string exceeded its memory budget");
		}
		peak_memory = std::max(peak_memory, co_live);
		charged_capacity = actual;
		ObserveCurrentMemory();
	}

	void AppendStringByte(std::string &result, char value, const JsonColumnPlan *column, uint64_t &charged_capacity) {
		ReserveRetainedString(result, 1, column, charged_capacity);
		result.push_back(value);
	}

	void AppendStringBytes(std::string &result, const std::string &source, std::size_t begin, std::size_t count,
	                       const JsonColumnPlan *column, uint64_t &charged_capacity) {
		ReserveRetainedString(result, count, column, charged_capacity);
		result.append(source, begin, count);
	}

	std::string ParseString(const JsonColumnPlan *retained_column = nullptr) {
		Expect('"');
		std::string result;
		uint64_t charged_capacity = 0;
		ReserveRetainedString(result, 0, retained_column, charged_capacity);
		while (position < input.size()) {
			Check();
			const auto character = input[position++];
			if (character == '"') {
				return result;
			}
			if (static_cast<unsigned char>(character) < 0x20) {
				MalformedJson();
			}
			if (character != '\\') {
				AppendValidatedUtf8(character, result, retained_column, charged_capacity);
				continue;
			}
			if (position >= input.size()) {
				MalformedJson();
			}
			const auto escaped = input[position++];
			switch (escaped) {
			case '"':
			case '\\':
			case '/':
				AppendStringByte(result, escaped, retained_column, charged_capacity);
				break;
			case 'b':
				AppendStringByte(result, '\b', retained_column, charged_capacity);
				break;
			case 'f':
				AppendStringByte(result, '\f', retained_column, charged_capacity);
				break;
			case 'n':
				AppendStringByte(result, '\n', retained_column, charged_capacity);
				break;
			case 'r':
				AppendStringByte(result, '\r', retained_column, charged_capacity);
				break;
			case 't':
				AppendStringByte(result, '\t', retained_column, charged_capacity);
				break;
			case 'u':
				AppendEscapedUnicode(result, retained_column, charged_capacity);
				break;
			default:
				MalformedJson();
			}
		}
		MalformedJson();
	}

	uint32_t ParseHexCodeUnit() {
		uint32_t result = 0;
		for (std::size_t index = 0; index < 4; index++) {
			Check();
			if (position >= input.size()) {
				MalformedJson();
			}
			const auto character = input[position++];
			result <<= 4;
			if (character >= '0' && character <= '9') {
				result += static_cast<uint32_t>(character - '0');
			} else if (character >= 'a' && character <= 'f') {
				result += static_cast<uint32_t>(character - 'a' + 10);
			} else if (character >= 'A' && character <= 'F') {
				result += static_cast<uint32_t>(character - 'A' + 10);
			} else {
				MalformedJson();
			}
		}
		return result;
	}

	void AppendCodePoint(uint32_t code_point, std::string &result, const JsonColumnPlan *column,
	                     uint64_t &charged_capacity) {
		char encoded[4];
		std::size_t count = 0;
		if (code_point <= 0x7f) {
			encoded[count++] = static_cast<char>(code_point);
		} else if (code_point <= 0x7ff) {
			encoded[count++] = static_cast<char>(0xc0 | (code_point >> 6));
			encoded[count++] = static_cast<char>(0x80 | (code_point & 0x3f));
		} else if (code_point <= 0xffff) {
			encoded[count++] = static_cast<char>(0xe0 | (code_point >> 12));
			encoded[count++] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
			encoded[count++] = static_cast<char>(0x80 | (code_point & 0x3f));
		} else {
			encoded[count++] = static_cast<char>(0xf0 | (code_point >> 18));
			encoded[count++] = static_cast<char>(0x80 | ((code_point >> 12) & 0x3f));
			encoded[count++] = static_cast<char>(0x80 | ((code_point >> 6) & 0x3f));
			encoded[count++] = static_cast<char>(0x80 | (code_point & 0x3f));
		}
		ReserveRetainedString(result, count, column, charged_capacity);
		result.append(encoded, count);
	}

	void AppendEscapedUnicode(std::string &result, const JsonColumnPlan *column, uint64_t &charged_capacity) {
		auto code_point = ParseHexCodeUnit();
		if (code_point >= 0xd800 && code_point <= 0xdbff) {
			if (position + 2 > input.size() || input[position] != '\\' || input[position + 1] != 'u') {
				MalformedJson();
			}
			position += 2;
			const auto low = ParseHexCodeUnit();
			if (low < 0xdc00 || low > 0xdfff) {
				MalformedJson();
			}
			code_point = 0x10000 + ((code_point - 0xd800) << 10) + (low - 0xdc00);
		} else if (code_point >= 0xdc00 && code_point <= 0xdfff) {
			MalformedJson();
		}
		AppendCodePoint(code_point, result, column, charged_capacity);
	}

	void AppendValidatedUtf8(char first_character, std::string &result, const JsonColumnPlan *column,
	                         uint64_t &charged_capacity) {
		const auto first = static_cast<unsigned char>(first_character);
		if (first < 0x80) {
			AppendStringByte(result, first_character, column, charged_capacity);
			return;
		}

		std::size_t continuation_count = 0;
		uint32_t code_point = 0;
		uint32_t minimum = 0;
		if ((first & 0xe0) == 0xc0) {
			continuation_count = 1;
			code_point = first & 0x1f;
			minimum = 0x80;
		} else if ((first & 0xf0) == 0xe0) {
			continuation_count = 2;
			code_point = first & 0x0f;
			minimum = 0x800;
		} else if ((first & 0xf8) == 0xf0) {
			continuation_count = 3;
			code_point = first & 0x07;
			minimum = 0x10000;
		} else {
			MalformedJson();
		}

		const auto begin = position - 1;
		for (std::size_t index = 0; index < continuation_count; index++) {
			Check();
			if (position >= input.size()) {
				MalformedJson();
			}
			const auto continuation = static_cast<unsigned char>(input[position++]);
			if ((continuation & 0xc0) != 0x80) {
				MalformedJson();
			}
			code_point = (code_point << 6) | (continuation & 0x3f);
		}
		if (code_point < minimum || code_point > 0x10ffff || (code_point >= 0xd800 && code_point <= 0xdfff)) {
			MalformedJson();
		}
		AppendStringBytes(result, input, begin, continuation_count + 1, column, charged_capacity);
	}

	std::string ParseNumberToken() {
		const auto begin = position;
		if (Peek() == '-') {
			position++;
		}
		if (Peek() == '0') {
			position++;
			if (Peek() >= '0' && Peek() <= '9') {
				MalformedJson();
			}
		} else {
			if (Peek() < '1' || Peek() > '9') {
				MalformedJson();
			}
			while (Peek() >= '0' && Peek() <= '9') {
				Check();
				position++;
			}
		}
		if (Peek() == '.') {
			position++;
			if (Peek() < '0' || Peek() > '9') {
				MalformedJson();
			}
			while (Peek() >= '0' && Peek() <= '9') {
				Check();
				position++;
			}
		}
		if (Peek() == 'e' || Peek() == 'E') {
			position++;
			if (Peek() == '+' || Peek() == '-') {
				position++;
			}
			if (Peek() < '0' || Peek() > '9') {
				MalformedJson();
			}
			while (Peek() >= '0' && Peek() <= '9') {
				Check();
				position++;
			}
		}
		return input.substr(begin, position - begin);
	}

	void ParseLiteral(const char *literal) {
		for (std::size_t index = 0; literal[index] != '\0'; index++) {
			Expect(literal[index]);
		}
	}

	void SkipValue(uint64_t depth = 0) {
		SkipWhitespace();
		if (depth > plan.max_json_nesting) {
			throw ExecutionError(ErrorStage::RESOURCE, "json_nesting",
			                     "HTTP response exceeded its JSON nesting budget");
		}
		if (Peek() == '"') {
			ParseString();
			return;
		}
		if (Peek() == '-' || (Peek() >= '0' && Peek() <= '9')) {
			ParseNumberToken();
			return;
		}
		if (Peek() == 't') {
			ParseLiteral("true");
			return;
		}
		if (Peek() == 'f') {
			ParseLiteral("false");
			return;
		}
		if (Peek() == 'n') {
			ParseLiteral("null");
			return;
		}
		if (Peek() == '[') {
			Expect('[');
			SkipWhitespace();
			while (Peek() != ']') {
				SkipValue(depth + 1);
				ConsumeArraySeparator();
			}
			Expect(']');
			return;
		}
		if (Peek() == '{') {
			Expect('{');
			SkipWhitespace();
			while (Peek() != '}') {
				RequireObjectKey();
				ParseString();
				SkipWhitespace();
				Expect(':');
				SkipValue(depth + 1);
				ConsumeObjectSeparator();
			}
			Expect('}');
			return;
		}
		MalformedJson();
	}

	int64_t ParseBigInt(const JsonColumnPlan &column) {
		SkipWhitespace();
		if (Peek() != '-' && (Peek() < '0' || Peek() > '9')) {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		const auto token = ParseNumberToken();
		if (token.find_first_of(".eE") != std::string::npos) {
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		errno = 0;
		char *end = nullptr;
		const auto value = std::strtoll(token.c_str(), &end, 10);
		if (errno == ERANGE || !end || *end != '\0') {
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field is outside the BIGINT range");
		}
		return static_cast<int64_t>(value);
	}

	double ParseDouble(const JsonColumnPlan &column) {
		SkipWhitespace();
		if (Peek() != '-' && (Peek() < '0' || Peek() > '9')) {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		// Unlike ParseBigInt, a fractional part and/or exponent are accepted:
		// DOUBLE has no integrality requirement.
		const auto token = ParseNumberToken();
		errno = 0;
		char *end = nullptr;
		const auto value = std::strtod(token.c_str(), &end);
		if (!end || *end != '\0') {
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		// RFC 0020: strtod sets ERANGE for both true overflow (HUGE_VAL) and
		// benign underflow to a subnormal or exact zero. Reject only true
		// overflow; a legitimate tiny measurement must decode, not fail.
		if (value == HUGE_VAL || value == -HUGE_VAL) {
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field is outside the DOUBLE range");
		}
		return value == 0.0 ? 0.0 : value;
	}

	std::string ParseVarchar(const JsonColumnPlan &column) {
		SkipWhitespace();
		if (Peek() != '"') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		auto result = ParseString(&column);
		if (static_cast<uint64_t>(result.size()) > plan.max_string_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, column.output_name,
			                     "required response string exceeded its byte budget");
		}
		return result;
	}

	bool ParseBoolean(const JsonColumnPlan &column) {
		SkipWhitespace();
		if (Peek() == 't') {
			ParseLiteral("true");
			return true;
		}
		if (Peek() == 'f') {
			ParseLiteral("false");
			return false;
		}
		SkipValue();
		throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
		                     "required response field has an incompatible type");
	}

	TypedScalarValue ParseScalarElement(const JsonColumnPlan &column) {
		switch (column.type.element_kind) {
		case ValueKind::BIGINT:
			return TypedScalarValue::BigInt(ParseBigInt(column));
		case ValueKind::VARCHAR:
			return TypedScalarValue::Varchar(ParseVarchar(column));
		case ValueKind::BOOLEAN:
			return TypedScalarValue::Boolean(ParseBoolean(column));
		case ValueKind::DOUBLE:
			return TypedScalarValue::Double(ParseDouble(column));
		}
		throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder received an invalid array element type");
	}

	void ReserveArrayElement(std::vector<TypedScalarValue> &elements) {
		if (elements.size() < elements.capacity()) {
			return;
		}
		const auto current = static_cast<uint64_t>(elements.capacity());
		uint64_t requested = 1;
		if (current != 0 && !CheckedMultiply(current, 2, requested)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded array exceeded its memory budget");
		}
		uint64_t requested_bytes = 0;
		uint64_t current_bytes = 0;
		uint64_t growth = 0;
		uint64_t retained = 0;
		if (!CheckedMultiply(requested, static_cast<uint64_t>(sizeof(TypedScalarValue)), requested_bytes) ||
		    !CheckedMultiply(current, static_cast<uint64_t>(sizeof(TypedScalarValue)), current_bytes) ||
		    requested_bytes < current_bytes || !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || !CheckedAdd(retained, requested_bytes, retained) ||
		    retained > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded array exceeded its memory budget");
		}
		peak_memory = std::max(peak_memory, retained);
		try {
			elements.reserve(static_cast<std::size_t>(requested));
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded array could not be allocated within its memory budget");
		}
		uint64_t actual_bytes = 0;
		uint64_t co_live = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(elements.capacity()),
		                     static_cast<uint64_t>(sizeof(TypedScalarValue)), actual_bytes) ||
		    actual_bytes < current_bytes || !CheckedAdd(decoded_memory, pending_string_memory, co_live) ||
		    !CheckedAdd(co_live, pending_array_memory, co_live) ||
		    !CheckedAdd(co_live, pending_structural_memory, co_live) ||
		    !CheckedAdd(co_live, continuation_memory, co_live) || !CheckedAdd(co_live, actual_bytes, co_live) ||
		    co_live > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded array exceeded its memory budget");
		}
		peak_memory = std::max(peak_memory, co_live);
		growth = actual_bytes - current_bytes;
		if (!CheckedAdd(pending_array_memory, growth, pending_array_memory) ||
		    !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, continuation_memory, retained) || retained > plan.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded array exceeded its memory budget");
		}
		ObserveCurrentMemory();
	}

	void ParseArray(const JsonColumnPlan &column, std::vector<TypedScalarValue> &elements) {
		SkipWhitespace();
		if (Peek() != '[') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
			                     "required response field has an incompatible type");
		}
		Expect('[');
		SkipWhitespace();
		while (Peek() != ']') {
			Check();
			ReserveArrayElement(elements);
			if (Peek() == 'n') {
				ParseLiteral("null");
				if (!column.type.element_nullable) {
					throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
					                     "required response array contains a null element");
				}
				elements.push_back(TypedScalarValue::Null(column.type.element_kind));
			} else {
				elements.push_back(ParseScalarElement(column));
			}
			ConsumeArraySeparator();
		}
		Expect(']');
	}

	void ParseColumnValue(std::size_t column_index, std::vector<ParsedSlot> &slots) {
		auto &slot = slots[column_index];
		const auto &column = plan.columns[column_index];
		if (slot.seen) {
			throw ExecutionError(ErrorStage::SCHEMA, column.output_name, "required response field is duplicated");
		}
		SkipWhitespace();
		if (Peek() == 'n') {
			ParseLiteral("null");
			slot.valid = false;
			slot.seen = true;
			if (!column.nullable) {
				throw ExecutionError(ErrorStage::SCHEMA, column.output_name,
				                     "required response field has an incompatible type");
			}
			return;
		}
		if (column.type.shape == ValueShape::ARRAY) {
			ParseArray(column, slot.elements);
			slot.seen = true;
			return;
		}
		switch (column.type.element_kind) {
		case ValueKind::BIGINT:
			slot.bigint_value = ParseBigInt(column);
			break;
		case ValueKind::VARCHAR:
			slot.varchar_value = ParseVarchar(column);
			break;
		case ValueKind::BOOLEAN:
			slot.boolean_value = ParseBoolean(column);
			break;
		case ValueKind::DOUBLE:
			slot.double_value = ParseDouble(column);
			break;
		}
		slot.seen = true;
	}

	void ParseSelectedObject(std::vector<ParsedSlot> &slots, const std::vector<std::string> &prefix) {
		Expect('{');
		SkipWhitespace();
		std::set<std::string> selected_keys;
		while (Peek() != '}') {
			RequireObjectKey();
			const auto key = ParseString();
			SkipWhitespace();
			Expect(':');

			auto path = prefix;
			path.push_back(key);
			std::size_t exact = plan.columns.size();
			std::size_t descendant = plan.columns.size();
			for (std::size_t index = 0; index < plan.columns.size(); index++) {
				const auto &candidate = plan.columns[index].json_path;
				if (!IsPathPrefix(path, candidate)) {
					continue;
				}
				if (candidate.size() == path.size()) {
					exact = index;
				} else if (descendant == plan.columns.size()) {
					descendant = index;
				}
			}

			if (exact == plan.columns.size() && descendant == plan.columns.size()) {
				SkipValue();
			} else {
				const auto column_index = exact != plan.columns.size() ? exact : descendant;
				if (!selected_keys.insert(key).second) {
					throw ExecutionError(ErrorStage::SCHEMA, plan.columns[column_index].output_name,
					                     "required response field is duplicated");
				}
				if (exact != plan.columns.size()) {
					ParseColumnValue(exact, slots);
				} else {
					SkipWhitespace();
					if (Peek() != '{') {
						SkipValue();
						throw ExecutionError(ErrorStage::SCHEMA, plan.columns[descendant].output_name,
						                     "required response field has an incompatible type");
					}
					ParseSelectedObject(slots, path);
				}
			}
			ConsumeObjectSeparator();
		}
		Expect('}');
	}

	TypedRow ParseRecord() {
		if (Peek() != '{') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, "", "response record does not match the declared schema");
		}
		uint64_t reserved_slot_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(plan.columns.size()), static_cast<uint64_t>(sizeof(ParsedSlot)),
		                     reserved_slot_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded row staging exceeded its memory budget");
		}
		ReserveStructuralMemory(reserved_slot_storage);
		std::vector<ParsedSlot> slots;
		try {
			slots.resize(plan.columns.size());
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded row staging could not be allocated within its memory budget");
		}
		uint64_t slot_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(slots.capacity()), static_cast<uint64_t>(sizeof(ParsedSlot)),
		                     slot_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded row staging exceeded its memory budget");
		}
		ReconcileStructuralMemory(reserved_slot_storage, slot_storage);
		ParseSelectedObject(slots, {});

		TypedRow row;
		uint64_t reserved_value_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(plan.columns.size()), static_cast<uint64_t>(sizeof(TypedValue)),
		                     reserved_value_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		ReserveStructuralMemory(reserved_value_storage);
		try {
			row.values.reserve(plan.columns.size());
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows could not be allocated within their memory budget");
		}
		uint64_t value_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(row.values.capacity()), static_cast<uint64_t>(sizeof(TypedValue)),
		                     value_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		ReconcileStructuralMemory(reserved_value_storage, value_storage);
		uint64_t row_memory = value_storage;
		uint64_t row_string_memory = 0;
		uint64_t row_array_memory = 0;
		for (std::size_t index = 0; index < plan.columns.size(); index++) {
			if (!slots[index].seen) {
				throw ExecutionError(ErrorStage::SCHEMA, plan.columns[index].output_name,
				                     "required response field is missing");
			}
			if (!slots[index].valid) {
				row.values.push_back(TypedValue::Null(plan.columns[index].type));
				continue;
			}
			if (plan.columns[index].type.shape == ValueShape::ARRAY) {
				auto value =
				    TypedValue::Array(plan.columns[index].type.element_kind, plan.columns[index].type.element_nullable,
				                      std::move(slots[index].elements));
				uint64_t element_storage = 0;
				if (!CheckedMultiply(static_cast<uint64_t>(value.elements.capacity()),
				                     static_cast<uint64_t>(sizeof(TypedScalarValue)), element_storage)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "decoded rows exceeded their memory budget");
				}
				uint64_t updated = 0;
				RequireMemory(row_memory, element_storage, plan.max_decoded_memory_bytes, updated);
				row_memory = updated;
				if (!CheckedAdd(row_array_memory, element_storage, row_array_memory)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "decoded rows exceeded their memory budget");
				}
				for (const auto &element : value.elements) {
					if (element.kind == ValueKind::VARCHAR && element.valid) {
						const auto bytes = static_cast<uint64_t>(element.varchar_value.capacity());
						RequireMemory(row_memory, bytes, plan.max_decoded_memory_bytes, updated);
						row_memory = updated;
						if (!CheckedAdd(row_string_memory, bytes, row_string_memory)) {
							throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
							                     "decoded rows exceeded their memory budget");
						}
					}
				}
				row.values.push_back(std::move(value));
				continue;
			}
			switch (plan.columns[index].type.element_kind) {
			case ValueKind::BIGINT:
				row.values.push_back(TypedValue::BigInt(slots[index].bigint_value));
				break;
			case ValueKind::VARCHAR: {
				auto value = TypedValue::Varchar(std::move(slots[index].varchar_value));
				uint64_t updated = 0;
				const auto bytes = static_cast<uint64_t>(value.varchar_value.capacity());
				RequireMemory(row_memory, bytes, plan.max_decoded_memory_bytes, updated);
				row_memory = updated;
				if (!CheckedAdd(row_string_memory, bytes, row_string_memory)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "decoded rows exceeded their memory budget");
				}
				row.values.push_back(std::move(value));
				break;
			}
			case ValueKind::BOOLEAN:
				row.values.push_back(TypedValue::Boolean(slots[index].boolean_value));
				break;
			case ValueKind::DOUBLE:
				row.values.push_back(TypedValue::Double(slots[index].double_value));
				break;
			}
		}
		uint64_t updated = 0;
		if (row_string_memory > pending_string_memory || row_array_memory > pending_array_memory ||
		    value_storage > pending_structural_memory || slot_storage > pending_structural_memory - value_storage) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "decoded rows exceeded their memory budget");
		}
		RequireMemory(decoded_memory, row_memory, plan.max_decoded_memory_bytes, updated);
		decoded_memory = updated;
		pending_string_memory -= row_string_memory;
		pending_array_memory -= row_array_memory;
		pending_structural_memory -= value_storage;
		pending_structural_memory -= slot_storage;
		return row;
	}

	void ParseRecords(std::vector<TypedRow> &result) {
		Expect('[');
		SkipWhitespace();
		while (Peek() != ']') {
			Check();
			if (static_cast<uint64_t>(result.size()) >= plan.max_records) {
				const auto path = EffectiveRecordsPath();
				throw ExecutionError(
				    ErrorStage::RESOURCE,
				    plan.response_source == JsonResponseSource::JSON_PATH_MANY ? RecordsDiagnosticField(path) : "",
				    "HTTP response exceeded its record budget");
			}
			result.push_back(ParseRecord());
			ConsumeArraySeparator();
		}
		Expect(']');
	}

	const std::string &input;
	const JsonDecodePlan &plan;
	ExecutionControl &control;
	std::size_t position;
	uint64_t decoded_memory;
	uint64_t pending_string_memory;
	uint64_t pending_array_memory;
	uint64_t pending_structural_memory;
	uint64_t continuation_memory;
	uint64_t peak_memory;
	std::string next_url;
};

} // namespace

DecodedJsonPage DecodeJsonPage(const std::string &body, const JsonDecodePlan &plan, ExecutionControl &control) {
	ValidatePlan(plan);
	Checkpoint(control, plan.deadline);
	try {
		JsonParser parser(body, plan, control);
		auto rows = parser.ParseResponse();
		DecodedJsonPage page {std::move(rows), parser.TakeNextUrl(), parser.RetainedMemoryBytes(), 0, 0};
		const auto continuation_memory =
		    parser.ContinuationMemoryBytes() == 0 ? 0 : static_cast<uint64_t>(page.next_url.capacity());
		parser.ReconcileTakenContinuationMemory(continuation_memory);
		page.continuation_memory_bytes = parser.ContinuationMemoryBytes();
		page.peak_memory_bytes = parser.PeakMemoryBytes();
		return page;
	} catch (const ExecutionCancelled &) {
		throw;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "decoded rows could not be allocated within their memory budget");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "JSON decoder failed");
	}
}

} // namespace internal
} // namespace cuac
