#include "cuac/internal/runtime/decoding/decoded_page_buffer.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <vector>

int main() {
	try {
		const auto handoff = cuac::internal::TypedBatchHandoffMemoryBytes(2, 4);
		cuac::internal::RequireTypedBatchHandoffMemory(100, 100 + handoff, 2, 4);
		bool one_under_rejected = false;
		try {
			cuac::internal::RequireTypedBatchHandoffMemory(100, 99 + handoff, 2, 4);
		} catch (const cuac::ExecutionError &error) {
			one_under_rejected = error.Stage() == cuac::ErrorStage::RESOURCE && error.Field() == "decoded_memory_bytes";
		}
		cuac_test::Require(one_under_rejected,
		                   "typed batch handoff accepted one byte below its co-live structural capacity");

		cuac::internal::DecodedPageBuffer buffer;
		std::vector<cuac::TypedRow> page(100);
		buffer.Install(std::move(page));
		cuac_test::Require(buffer.Rows().size() == 100 && buffer.Capacity() >= 100,
		                   "decoded page buffer did not own the installed page allocation");
		buffer.Release();
		cuac_test::Require(buffer.Rows().empty() && buffer.Capacity() == 0,
		                   "decoded page release retained prior vector capacity");
		buffer.Release();
		cuac_test::Require(buffer.Capacity() == 0, "decoded page release was not idempotent");
		std::cout << "decoded page buffer tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "decoded page buffer tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
