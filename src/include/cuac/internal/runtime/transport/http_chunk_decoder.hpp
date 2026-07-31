#pragma once

#include "cuac/runtime/execution.hpp"

#include <chrono>
#include <cstdint>
#include <exception>
#include <string>

namespace cuac {
namespace internal {

// Safe parser failure for HTTP/1.1 chunk framing. The decoder never exposes
// received extensions, trailers, or payload bytes through its diagnostic.
class HttpChunkDecodeError : public std::exception {
public:
	const char *what() const noexcept override;
};

// Removes one already wire-bounded HTTP/1.1 chunked coding. Chunk extensions
// and trailers are grammar-validated and discarded; they grant no product
// metadata authority. The returned identity body cannot exceed max_body_bytes.
std::string DecodeHttpChunkedBody(const std::string &encoded, uint64_t max_body_bytes, ExecutionControl &control,
                                  std::chrono::steady_clock::time_point deadline);

} // namespace internal
} // namespace cuac
