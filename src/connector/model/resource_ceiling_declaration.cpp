#include "cuac/connector/catalog.hpp"
#include "cuac/internal/connector/model/resource_ceiling_declaration.hpp"

#include <ostream>
#include <stdexcept>

namespace cuac {

CompiledResourceCeilings::CompiledResourceCeilings(std::uint64_t max_records_p,
                                                   std::uint64_t max_extracted_string_bytes_p)
    : has_response_byte_narrowing(false), max_response_bytes_per_page(0), max_response_bytes_per_scan(0),
      max_records_per_page(max_records_p), max_records_per_scan(max_records_p),
      max_extracted_string_bytes(max_extracted_string_bytes_p) {
}

CompiledResourceCeilings::CompiledResourceCeilings(std::uint64_t max_response_bytes_per_page_p,
                                                   std::uint64_t max_response_bytes_per_scan_p,
                                                   std::uint64_t max_records_per_page_p,
                                                   std::uint64_t max_records_per_scan_p,
                                                   std::uint64_t max_extracted_string_bytes_p)
    : has_response_byte_narrowing(true), max_response_bytes_per_page(max_response_bytes_per_page_p),
      max_response_bytes_per_scan(max_response_bytes_per_scan_p), max_records_per_page(max_records_per_page_p),
      max_records_per_scan(max_records_per_scan_p), max_extracted_string_bytes(max_extracted_string_bytes_p) {
}

bool CompiledResourceCeilings::HasResponseByteNarrowing() const {
	return has_response_byte_narrowing;
}

void CompiledResourceCeilings::RequireResponseByteNarrowing() const {
	if (!has_response_byte_narrowing) {
		throw std::logic_error("compiled resources inherit the connector response-byte policy");
	}
}

std::uint64_t CompiledResourceCeilings::MaxResponseBytesPerPage() const {
	RequireResponseByteNarrowing();
	return max_response_bytes_per_page;
}

std::uint64_t CompiledResourceCeilings::MaxResponseBytesPerScan() const {
	RequireResponseByteNarrowing();
	return max_response_bytes_per_scan;
}

std::uint64_t CompiledResourceCeilings::MaxRecordsPerPage() const {
	return max_records_per_page;
}

std::uint64_t CompiledResourceCeilings::MaxRecordsPerScan() const {
	return max_records_per_scan;
}

std::uint64_t CompiledResourceCeilings::MaxExtractedStringBytes() const {
	return max_extracted_string_bytes;
}

namespace internal {

void ValidateResourceCeilingsValue(const CompiledResourceCeilings &ceilings) {
	if (ceilings.MaxRecordsPerPage() == 0 || ceilings.MaxRecordsPerScan() == 0 ||
	    ceilings.MaxExtractedStringBytes() == 0 || ceilings.MaxRecordsPerScan() < ceilings.MaxRecordsPerPage()) {
		throw std::invalid_argument("compiled relation contains an empty or inconsistent record ceiling");
	}
	if (ceilings.HasResponseByteNarrowing() &&
	    (ceilings.MaxResponseBytesPerPage() == 0 || ceilings.MaxResponseBytesPerScan() == 0 ||
	     ceilings.MaxResponseBytesPerScan() < ceilings.MaxResponseBytesPerPage())) {
		throw std::invalid_argument("compiled relation contains an inconsistent response-byte ceiling");
	}
}

void AppendResourceCeilings(std::ostream &result, const CompiledResourceCeilings &ceilings) {
	// Preserve the compact explanation for unscoped one-page declarations while
	// making every explicitly scoped or paginated declaration unambiguous.
	if (!ceilings.HasResponseByteNarrowing() && ceilings.MaxRecordsPerPage() == ceilings.MaxRecordsPerScan()) {
		result << "records:" << ceilings.MaxRecordsPerPage()
		       << ",extracted_string_bytes:" << ceilings.MaxExtractedStringBytes();
		return;
	}
	result << "response_bytes_per_page:" << ceilings.MaxResponseBytesPerPage()
	       << ",response_bytes_per_scan:" << ceilings.MaxResponseBytesPerScan()
	       << ",records_per_page:" << ceilings.MaxRecordsPerPage()
	       << ",records_per_scan:" << ceilings.MaxRecordsPerScan()
	       << ",extracted_string_bytes:" << ceilings.MaxExtractedStringBytes();
}

} // namespace internal
} // namespace cuac
