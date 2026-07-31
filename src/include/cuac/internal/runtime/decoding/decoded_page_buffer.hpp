#pragma once

#include "cuac/runtime/execution.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace cuac {
namespace internal {

inline uint64_t TypedBatchHandoffMemoryBytes(std::size_t row_capacity, std::size_t column_capacity) {
	const auto rows = static_cast<uint64_t>(row_capacity);
	const auto columns = static_cast<uint64_t>(column_capacity);
	if ((rows != 0 && rows > std::numeric_limits<uint64_t>::max() / sizeof(TypedRow)) ||
	    (columns != 0 && columns > std::numeric_limits<uint64_t>::max() / sizeof(OutputValueType))) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "typed batch handoff exceeded its decoded-memory budget");
	}
	const auto row_bytes = rows * sizeof(TypedRow);
	const auto column_bytes = columns * sizeof(OutputValueType);
	if (column_bytes > std::numeric_limits<uint64_t>::max() - row_bytes) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "typed batch handoff exceeded its decoded-memory budget");
	}
	return row_bytes + column_bytes;
}

// The decoded page stays live while Runtime constructs a non-observable
// staging batch. Debit both outer vectors before allocation; inner row-owned
// storage is moved, not copied, and is already included in retained_page_bytes.
inline void RequireTypedBatchHandoffMemory(uint64_t retained_page_bytes, uint64_t decoded_memory_allowance,
                                           std::size_t row_capacity, std::size_t column_capacity) {
	const auto handoff_bytes = TypedBatchHandoffMemoryBytes(row_capacity, column_capacity);
	if (retained_page_bytes > decoded_memory_allowance ||
	    handoff_bytes > decoded_memory_allowance - retained_page_bytes) {
		throw ExecutionError(ErrorStage::RESOURCE, "decoded_memory_bytes",
		                     "typed batch handoff exceeded its decoded-memory budget");
	}
}

// Scan-owned storage for exactly one decoded page. Release swaps with an empty
// vector so row objects and vector capacity are returned before accounting
// authorizes another page decode; clear() alone would retain the old page's
// allocation and permit two page buffers to overlap invisibly.
class DecodedPageBuffer {
public:
	std::vector<TypedRow> &Rows() noexcept {
		return rows;
	}

	const std::vector<TypedRow> &Rows() const noexcept {
		return rows;
	}

	void Install(std::vector<TypedRow> incoming) {
		rows = std::move(incoming);
	}

	void Release() noexcept {
		std::vector<TypedRow> empty;
		rows.swap(empty);
	}

	std::size_t Capacity() const noexcept {
		return rows.capacity();
	}

private:
	std::vector<TypedRow> rows;
};

} // namespace internal
} // namespace cuac
