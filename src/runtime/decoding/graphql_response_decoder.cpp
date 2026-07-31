#include "cuac/internal/runtime/decoding/graphql_response_decoder.hpp"
#include "cuac/internal/runtime/decoding/strict_json_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <utility>

namespace cuac {
namespace internal {
namespace {

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

class GraphqlJsonParser : private StrictJsonReader, private StrictJsonStringCapacityObserver {
public:
	GraphqlJsonParser(const std::string &input, const AdmittedGraphqlRequestProfile &profile_p,
	                  const GraphqlDecodeLimits &limits_p, ExecutionControl &control)
	    : StrictJsonReader(input, limits_p.max_json_nesting, limits_p.deadline, control), profile(profile_p),
	      limits(limits_p), decoded_memory(0), pending_string_memory(0), pending_array_memory(0),
	      pending_structural_memory(0), peak_memory(0), has_next(false), end_cursor_is_null(false) {
	}

	DecodedGraphqlPage Parse() {
		ValidateProfile();
		ValidateDocument();
		CheckErrors();
		auto rows = ParseRows();
		ParseHasNext();
		ParseEndCursor();
		if (has_next && (end_cursor_is_null || end_cursor.empty())) {
			throw ExecutionError(ErrorStage::SCHEMA, "pagination.end_cursor",
			                     "GraphQL continuation cursor is missing or empty");
		}
		const auto cursor_memory = has_next ? static_cast<uint64_t>(end_cursor.capacity()) : 0;
		return {std::move(rows), has_next, std::move(end_cursor), decoded_memory, cursor_memory, peak_memory};
	}

private:
	struct Slot {
		Slot() : seen(false), valid(true), bigint_value(0), boolean_value(false), double_value(0.0) {
		}
		bool seen;
		bool valid;
		int64_t bigint_value;
		std::string varchar_value;
		bool boolean_value;
		double double_value;
		std::vector<TypedScalarValue> elements;
	};

	void ValidateProfile() {
		if (limits.max_records == 0 || limits.max_records > 100 || limits.max_string_bytes == 0 ||
		    limits.max_string_bytes > 512 || limits.max_json_nesting == 0 || limits.max_json_nesting > 16 ||
		    limits.max_decoded_memory_bytes == 0 || profile.Columns().empty() || profile.Columns().size() > 256) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL decoder received an invalid profile");
		}
		for (const auto &column : profile.Columns()) {
			if (column.type.shape != ValueShape::SCALAR && column.type.shape != ValueShape::ARRAY) {
				throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL decoder received an invalid profile");
			}
			if (column.type.shape == ValueShape::SCALAR && column.type.element_nullable) {
				throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL decoder received an invalid profile");
			}
			switch (column.type.element_kind) {
			case ValueKind::BIGINT:
			case ValueKind::VARCHAR:
			case ValueKind::BOOLEAN:
			case ValueKind::DOUBLE:
				break;
			default:
				throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL decoder received an invalid profile");
			}
		}
	}

	std::string PathName(const std::vector<std::string> &path) const {
		std::string result;
		for (const auto &segment : path) {
			if (!result.empty()) {
				result += ".";
			}
			result += segment;
		}
		return result;
	}

	bool Traverse(const std::vector<std::string> &path, std::size_t index, bool required,
	              const std::function<void()> &leaf) {
		SkipWhitespace();
		if (Peek() != '{') {
			SkipValue();
			if (required) {
				throw ExecutionError(ErrorStage::SCHEMA, PathName(path),
				                     "required GraphQL response path is missing or invalid");
			}
			return false;
		}
		Expect('{');
		SkipWhitespace();
		bool found = false;
		while (Peek() != '}') {
			RequireObjectKey();
			const auto key = ParseObjectKey();
			SkipWhitespace();
			Expect(':');
			if (key.Equals(path[index])) {
				if (found) {
					throw ExecutionError(ErrorStage::SCHEMA, PathName(path), "GraphQL response path is duplicated");
				}
				found = true;
				if (index + 1 == path.size()) {
					leaf();
				} else {
					(void)Traverse(path, index + 1, required, leaf);
				}
			} else {
				SkipValue();
			}
			ObjectSeparator();
		}
		Expect('}');
		if (!found && required) {
			throw ExecutionError(ErrorStage::SCHEMA, PathName(path), "required GraphQL response path is missing");
		}
		return found;
	}

	void CheckErrors() {
		Reset();
		(void)Traverse(profile.ErrorsPath(), 0, false, [this]() {
			SkipWhitespace();
			if (Peek() != '[') {
				SkipValue();
				throw ExecutionError(ErrorStage::SCHEMA, PathName(profile.ErrorsPath()),
				                     "GraphQL errors field must be an array");
			}
			Expect('[');
			SkipWhitespace();
			const bool nonempty = Peek() != ']';
			while (Peek() != ']') {
				SkipValue();
				ArraySeparator();
			}
			Expect(']');
			if (nonempty) {
				throw ExecutionError(ErrorStage::REMOTE_PROTOCOL, "errors",
				                     "remote protocol response reported application errors");
			}
		});
	}

	std::vector<TypedRow> AllocateRows() {
		std::vector<TypedRow> rows;
		uint64_t requested_bytes = 0;
		if (!CheckedMultiply(limits.max_records, static_cast<uint64_t>(sizeof(TypedRow)), requested_bytes) ||
		    requested_bytes > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL rows exceeded their decoded-memory budget");
		}
		try {
			rows.reserve(static_cast<std::size_t>(limits.max_records));
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL rows exceeded their decoded-memory budget");
		}
		uint64_t bytes = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(rows.capacity()), static_cast<uint64_t>(sizeof(TypedRow)), bytes) ||
		    bytes > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL rows exceeded their decoded-memory budget");
		}
		decoded_memory = bytes;
		ObserveCurrentMemory();
		return rows;
	}

	void ObserveCurrentMemory() {
		uint64_t current = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, current) ||
		    !CheckedAdd(current, pending_array_memory, current) ||
		    !CheckedAdd(current, pending_structural_memory, current) || current > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL response exceeded its decoded-memory budget");
		}
		peak_memory = std::max(peak_memory, current);
	}

	void ReserveStringCapacity(uint64_t current_capacity, uint64_t requested_capacity) override {
		if (requested_capacity < current_capacity) {
			throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL string capacity accounting failed");
		}
		uint64_t retained = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, requested_capacity, retained) || retained > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL response exceeded its decoded-memory budget");
		}
		// Reallocation retains the old buffer until the prospective replacement
		// allocation succeeds, so the replacement is co-live rather than growth.
		peak_memory = std::max(peak_memory, retained);
	}

	void ReconcileStringCapacity(uint64_t current_capacity, uint64_t reserved_capacity,
	                             uint64_t actual_capacity) override {
		uint64_t co_live = 0;
		if (actual_capacity < reserved_capacity || current_capacity > pending_string_memory ||
		    !CheckedAdd(decoded_memory, pending_string_memory, co_live) ||
		    !CheckedAdd(co_live, pending_array_memory, co_live) ||
		    !CheckedAdd(co_live, pending_structural_memory, co_live) ||
		    !CheckedAdd(co_live, actual_capacity, co_live) || co_live > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL response exceeded its decoded-memory budget");
		}
		peak_memory = std::max(peak_memory, co_live);
		pending_string_memory -= current_capacity;
		if (!CheckedAdd(pending_string_memory, actual_capacity, pending_string_memory)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL response exceeded its decoded-memory budget");
		}
		ObserveCurrentMemory();
	}

	void ReserveStructuralMemory(uint64_t addition) {
		uint64_t retained = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) || !CheckedAdd(retained, addition, retained) ||
		    retained > limits.max_decoded_memory_bytes ||
		    !CheckedAdd(pending_structural_memory, addition, pending_structural_memory)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row staging exceeded its decoded-memory budget");
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

	std::vector<TypedRow> ParseRows() {
		auto rows = AllocateRows();
		Reset();
		(void)Traverse(profile.NodesPath(), 0, true, [this, &rows]() {
			SkipWhitespace();
			if (Peek() != '[') {
				SkipValue();
				throw ExecutionError(ErrorStage::SCHEMA, PathName(profile.NodesPath()),
				                     "required GraphQL nodes field must be an array");
			}
			Expect('[');
			SkipWhitespace();
			while (Peek() != ']') {
				if (static_cast<uint64_t>(rows.size()) >= limits.max_records) {
					throw ExecutionError(ErrorStage::RESOURCE, PathName(profile.NodesPath()),
					                     "GraphQL response exceeded its record budget");
				}
				const auto begin = Position();
				if (Peek() != '{') {
					SkipValue();
					throw ExecutionError(ErrorStage::SCHEMA, PathName(profile.NodesPath()),
					                     "GraphQL node does not match the declared schema");
				}
				SkipValue();
				const auto end = Position();
				rows.push_back(ParseNode(begin, end));
				SetPosition(end);
				ArraySeparator();
			}
			Expect(']');
		});
		return rows;
	}

	std::string ParseBoundedString(const std::string &field) {
		SkipWhitespace();
		if (Peek() != '"') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
		}
		uint64_t retained = 0;
		if (!CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) || retained > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL response exceeded its decoded-memory budget");
		}
		const auto remaining = limits.max_decoded_memory_bytes - retained;
		return ParseString(std::min(limits.max_string_bytes, remaining),
		                   remaining < limits.max_string_bytes ? "decoded_memory_bytes" : field,
		                   remaining < limits.max_string_bytes ? "GraphQL response exceeded its decoded-memory budget"
		                                                       : "GraphQL response string exceeded its byte budget",
		                   this);
	}

	int64_t ParseBigInt(const std::string &field) {
		SkipWhitespace();
		if (Peek() != '-' && (Peek() < '0' || Peek() > '9')) {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
		}
		const auto token = ParseNumberToken(32, field, "GraphQL integer exceeded its lexical byte budget");
		if (token.find_first_of(".eE") != std::string::npos) {
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
		}
		errno = 0;
		char *end = nullptr;
		const auto value = std::strtoll(token.c_str(), &end, 10);
		if (errno == ERANGE || !end || *end != '\0') {
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL integer is outside the BIGINT range");
		}
		return static_cast<int64_t>(value);
	}

	double ParseDouble(const std::string &field) {
		SkipWhitespace();
		if (Peek() != '-' && (Peek() < '0' || Peek() > '9')) {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
		}
		const auto token = ParseNumberToken(64, field, "GraphQL number exceeded its lexical byte budget");
		errno = 0;
		char *end = nullptr;
		const auto value = std::strtod(token.c_str(), &end);
		if (!end || *end != '\0') {
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
		}
		// RFC 0020: reject only true overflow; benign underflow to a
		// subnormal or exact zero is a legitimate, accepted result.
		if (value == HUGE_VAL || value == -HUGE_VAL) {
			throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL number is outside the DOUBLE range");
		}
		return value == 0.0 ? 0.0 : value;
	}

	bool ParseBoolean(const std::string &field) {
		SkipWhitespace();
		if (Peek() == 't') {
			Literal("true");
			return true;
		}
		if (Peek() == 'f') {
			Literal("false");
			return false;
		}
		SkipValue();
		throw ExecutionError(ErrorStage::SCHEMA, field, "GraphQL response field has an incompatible type");
	}

	TypedScalarValue ParseScalarElement(const AdmittedGraphqlColumn &column) {
		switch (column.type.element_kind) {
		case ValueKind::VARCHAR:
			return TypedScalarValue::Varchar(ParseBoundedString(column.name));
		case ValueKind::BIGINT:
			return TypedScalarValue::BigInt(ParseBigInt(column.name));
		case ValueKind::BOOLEAN:
			return TypedScalarValue::Boolean(ParseBoolean(column.name));
		case ValueKind::DOUBLE:
			return TypedScalarValue::Double(ParseDouble(column.name));
		}
		throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL decoder received an invalid array element type");
	}

	void ReserveArrayElement(std::vector<TypedScalarValue> &elements) {
		if (elements.size() < elements.capacity()) {
			return;
		}
		const auto current = static_cast<uint64_t>(elements.capacity());
		uint64_t requested = 1;
		if (current != 0 && !CheckedMultiply(current, 2, requested)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL array exceeded its decoded-memory budget");
		}
		uint64_t requested_bytes = 0;
		uint64_t current_bytes = 0;
		uint64_t retained = 0;
		if (!CheckedMultiply(requested, static_cast<uint64_t>(sizeof(TypedScalarValue)), requested_bytes) ||
		    !CheckedMultiply(current, static_cast<uint64_t>(sizeof(TypedScalarValue)), current_bytes) ||
		    requested_bytes < current_bytes || !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) ||
		    !CheckedAdd(retained, requested_bytes, retained) || retained > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL array exceeded its decoded-memory budget");
		}
		peak_memory = std::max(peak_memory, retained);
		try {
			elements.reserve(static_cast<std::size_t>(requested));
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL array exceeded available decoded memory");
		}
		uint64_t actual_bytes = 0;
		uint64_t co_live = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(elements.capacity()),
		                     static_cast<uint64_t>(sizeof(TypedScalarValue)), actual_bytes) ||
		    actual_bytes < current_bytes || !CheckedAdd(decoded_memory, pending_string_memory, co_live) ||
		    !CheckedAdd(co_live, pending_array_memory, co_live) ||
		    !CheckedAdd(co_live, pending_structural_memory, co_live) || !CheckedAdd(co_live, actual_bytes, co_live) ||
		    co_live > limits.max_decoded_memory_bytes ||
		    !CheckedAdd(pending_array_memory, actual_bytes - current_bytes, pending_array_memory) ||
		    !CheckedAdd(decoded_memory, pending_string_memory, retained) ||
		    !CheckedAdd(retained, pending_array_memory, retained) ||
		    !CheckedAdd(retained, pending_structural_memory, retained) || retained > limits.max_decoded_memory_bytes) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL array exceeded its decoded-memory budget");
		}
		peak_memory = std::max(peak_memory, co_live);
		ObserveCurrentMemory();
	}

	void ParseArray(const AdmittedGraphqlColumn &column, Slot &slot) {
		SkipWhitespace();
		if (Peek() != '[') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.name, "GraphQL response field has an incompatible type");
		}
		Expect('[');
		SkipWhitespace();
		while (Peek() != ']') {
			Check();
			ReserveArrayElement(slot.elements);
			if (Peek() == 'n') {
				Literal("null");
				if (!column.type.element_nullable) {
					throw ExecutionError(ErrorStage::SCHEMA, column.name,
					                     "GraphQL response array contains a null element");
				}
				slot.elements.push_back(TypedScalarValue::Null(column.type.element_kind));
			} else {
				slot.elements.push_back(ParseScalarElement(column));
			}
			ArraySeparator();
		}
		Expect(']');
	}

	void ParseColumn(const AdmittedGraphqlColumn &column, std::size_t path_index, Slot &slot) {
		SkipWhitespace();
		if (Peek() == 'n') {
			Literal("null");
			if (!column.nullable) {
				throw ExecutionError(ErrorStage::SCHEMA, column.name, "required GraphQL response field is null");
			}
			slot.seen = true;
			slot.valid = false;
			return;
		}
		if (Peek() != '{') {
			SkipValue();
			throw ExecutionError(ErrorStage::SCHEMA, column.name,
			                     "GraphQL response field has an incompatible parent type");
		}
		Expect('{');
		SkipWhitespace();
		bool found = false;
		while (Peek() != '}') {
			RequireObjectKey();
			const auto key = ParseObjectKey();
			SkipWhitespace();
			Expect(':');
			if (key.Equals(column.response_path[path_index])) {
				if (found) {
					throw ExecutionError(ErrorStage::SCHEMA, column.name, "GraphQL response field is duplicated");
				}
				found = true;
				if (path_index + 1 < column.response_path.size()) {
					ParseColumn(column, path_index + 1, slot);
				} else {
					SkipWhitespace();
					if (Peek() == 'n') {
						Literal("null");
						if (!column.nullable) {
							throw ExecutionError(ErrorStage::SCHEMA, column.name,
							                     "required GraphQL response field is null");
						}
						slot.valid = false;
					} else {
						if (column.type.shape == ValueShape::ARRAY) {
							ParseArray(column, slot);
						} else {
							switch (column.type.element_kind) {
							case ValueKind::VARCHAR:
								slot.varchar_value = ParseBoundedString(column.name);
								break;
							case ValueKind::BIGINT:
								slot.bigint_value = ParseBigInt(column.name);
								break;
							case ValueKind::BOOLEAN:
								slot.boolean_value = ParseBoolean(column.name);
								break;
							case ValueKind::DOUBLE:
								slot.double_value = ParseDouble(column.name);
								break;
							}
						}
					}
					slot.seen = true;
				}
			} else {
				SkipValue();
			}
			ObjectSeparator();
		}
		Expect('}');
		if (!found) {
			throw ExecutionError(ErrorStage::SCHEMA, column.name, "required GraphQL response field is missing");
		}
	}

	TypedRow ParseNode(std::size_t begin, std::size_t end) {
		uint64_t reserved_slot_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(profile.Columns().size()), static_cast<uint64_t>(sizeof(Slot)),
		                     reserved_slot_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row staging exceeded its decoded-memory budget");
		}
		ReserveStructuralMemory(reserved_slot_storage);
		std::vector<Slot> slots;
		try {
			slots.resize(profile.Columns().size());
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row staging exceeded available decoded memory");
		}
		uint64_t slot_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(slots.capacity()), static_cast<uint64_t>(sizeof(Slot)),
		                     slot_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row staging exceeded its decoded-memory budget");
		}
		ReconcileStructuralMemory(reserved_slot_storage, slot_storage);
		for (std::size_t index = 0; index < profile.Columns().size(); index++) {
			SetPosition(begin);
			ParseColumn(profile.Columns()[index], 0, slots[index]);
			if (Position() != end || !slots[index].seen) {
				throw ExecutionError(ErrorStage::SCHEMA, profile.Columns()[index].name,
				                     "required GraphQL response field is missing");
			}
		}
		TypedRow row;
		uint64_t reserved_value_storage = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(profile.Columns().size()), static_cast<uint64_t>(sizeof(TypedValue)),
		                     reserved_value_storage)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row exceeded its decoded-memory budget");
		}
		ReserveStructuralMemory(reserved_value_storage);
		try {
			row.values.reserve(profile.Columns().size());
		} catch (const std::bad_alloc &) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row exceeded available decoded memory");
		}
		uint64_t row_memory = 0;
		if (!CheckedMultiply(static_cast<uint64_t>(row.values.capacity()), static_cast<uint64_t>(sizeof(TypedValue)),
		                     row_memory)) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL row exceeded its decoded-memory budget");
		}
		ReconcileStructuralMemory(reserved_value_storage, row_memory);
		const auto value_storage = row_memory;
		uint64_t row_strings = 0;
		uint64_t row_arrays = 0;
		for (std::size_t index = 0; index < slots.size(); index++) {
			const auto &column = profile.Columns()[index];
			auto &slot = slots[index];
			if (!slot.valid) {
				row.values.push_back(TypedValue::Null(column.type));
			} else if (column.type.shape == ValueShape::ARRAY) {
				auto value =
				    TypedValue::Array(column.type.element_kind, column.type.element_nullable, std::move(slot.elements));
				uint64_t element_storage = 0;
				if (!CheckedMultiply(static_cast<uint64_t>(value.elements.capacity()),
				                     static_cast<uint64_t>(sizeof(TypedScalarValue)), element_storage)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "GraphQL row exceeded its decoded-memory budget");
				}
				uint64_t updated = 0;
				if (!CheckedAdd(row_memory, element_storage, updated) || updated > limits.max_decoded_memory_bytes) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "GraphQL row exceeded its decoded-memory budget");
				}
				row_memory = updated;
				if (!CheckedAdd(row_arrays, element_storage, row_arrays)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "GraphQL row exceeded its decoded-memory budget");
				}
				for (const auto &element : value.elements) {
					if (element.kind == ValueKind::VARCHAR && element.valid) {
						const auto bytes = static_cast<uint64_t>(element.varchar_value.capacity());
						if (!CheckedAdd(row_memory, bytes, updated) || updated > limits.max_decoded_memory_bytes ||
						    !CheckedAdd(row_strings, bytes, updated)) {
							throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
							                     "GraphQL row exceeded its decoded-memory budget");
						}
						row_memory += bytes;
						row_strings = updated;
					}
				}
				row.values.push_back(std::move(value));
			} else if (column.type.element_kind == ValueKind::VARCHAR) {
				auto value = TypedValue::Varchar(std::move(slot.varchar_value));
				uint64_t updated = 0;
				if (!CheckedAdd(row_memory, static_cast<uint64_t>(value.varchar_value.capacity()), updated) ||
				    updated > limits.max_decoded_memory_bytes) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "GraphQL row exceeded its decoded-memory budget");
				}
				row_memory = updated;
				if (!CheckedAdd(row_strings, static_cast<uint64_t>(value.varchar_value.capacity()), updated)) {
					throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
					                     "GraphQL row exceeded its decoded-memory budget");
				}
				row_strings = updated;
				row.values.push_back(std::move(value));
			} else if (column.type.element_kind == ValueKind::BIGINT) {
				row.values.push_back(TypedValue::BigInt(slot.bigint_value));
			} else if (column.type.element_kind == ValueKind::DOUBLE) {
				row.values.push_back(TypedValue::Double(slot.double_value));
			} else {
				row.values.push_back(TypedValue::Boolean(slot.boolean_value));
			}
		}
		uint64_t updated = 0;
		if (!CheckedAdd(decoded_memory, row_memory, updated) || updated > limits.max_decoded_memory_bytes ||
		    row_strings > pending_string_memory || row_arrays > pending_array_memory ||
		    value_storage > pending_structural_memory || slot_storage > pending_structural_memory - value_storage) {
			throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
			                     "GraphQL rows exceeded their decoded-memory budget");
		}
		decoded_memory = updated;
		pending_string_memory -= row_strings;
		pending_array_memory -= row_arrays;
		pending_structural_memory -= value_storage;
		pending_structural_memory -= slot_storage;
		return row;
	}

	void ParseHasNext() {
		Reset();
		(void)Traverse(profile.HasNextPagePath(), 0, true,
		               [this]() { has_next = ParseBoolean("pagination.has_next"); });
	}

	void ParseEndCursor() {
		Reset();
		(void)Traverse(profile.EndCursorPath(), 0, true, [this]() {
			SkipWhitespace();
			if (Peek() == 'n') {
				Literal("null");
				end_cursor_is_null = true;
				return;
			}
			end_cursor = ParseBoundedString("pagination.end_cursor");
		});
	}

	const AdmittedGraphqlRequestProfile &profile;
	const GraphqlDecodeLimits &limits;
	uint64_t decoded_memory;
	uint64_t pending_string_memory;
	uint64_t pending_array_memory;
	uint64_t pending_structural_memory;
	uint64_t peak_memory;
	bool has_next;
	bool end_cursor_is_null;
	std::string end_cursor;
};

} // namespace

DecodedGraphqlPage DecodeGraphqlResponse(const std::string &body, const AdmittedGraphqlRequestProfile &profile,
                                         const GraphqlDecodeLimits &limits, ExecutionControl &control) {
	Checkpoint(control, limits.deadline);
	try {
		GraphqlJsonParser parser(body, profile, limits, control);
		return parser.Parse();
	} catch (const ExecutionCancelled &) {
		throw;
	} catch (const ExecutionError &) {
		throw;
	} catch (const std::bad_alloc &) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "GraphQL response exceeded available decoded memory");
	} catch (...) {
		throw ExecutionError(ErrorStage::INTERNAL, "", "GraphQL response decoder failed");
	}
}

} // namespace internal
} // namespace cuac
