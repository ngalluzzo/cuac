#include "cuac/internal/connector/source/package_digest.hpp"

#include "cuac/connector/content_digest.hpp"
#include "cuac/internal/connector/compiler/package_cancellation.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <vector>

namespace cuac {
namespace connector {
namespace {

bool IsContinuation(unsigned char value) {
	return (value & 0xc0U) == 0x80U;
}

bool IsValidUtf8(const std::string &value) {
	for (std::size_t index = 0; index < value.size();) {
		const auto lead = static_cast<unsigned char>(value[index]);
		if (lead <= 0x7fU) {
			index++;
			continue;
		}
		if (lead >= 0xc2U && lead <= 0xdfU) {
			if (index + 1 >= value.size() || !IsContinuation(static_cast<unsigned char>(value[index + 1]))) {
				return false;
			}
			index += 2;
			continue;
		}
		if (lead >= 0xe0U && lead <= 0xefU) {
			if (index + 2 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			const auto third = static_cast<unsigned char>(value[index + 2]);
			if (!IsContinuation(second) || !IsContinuation(third) || (lead == 0xe0U && second < 0xa0U) ||
			    (lead == 0xedU && second >= 0xa0U)) {
				return false;
			}
			index += 3;
			continue;
		}
		if (lead >= 0xf0U && lead <= 0xf4U) {
			if (index + 3 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			if (!IsContinuation(second) || !IsContinuation(static_cast<unsigned char>(value[index + 2])) ||
			    !IsContinuation(static_cast<unsigned char>(value[index + 3])) || (lead == 0xf0U && second < 0x90U) ||
			    (lead == 0xf4U && second >= 0x90U)) {
				return false;
			}
			index += 4;
			continue;
		}
		return false;
	}
	return true;
}

bool IsNormalizedRelativePath(const std::string &path) {
	if (path.empty() || path[0] == '/' || path[path.size() - 1] == '/' || path.find('\\') != std::string::npos ||
	    path.find('\0') != std::string::npos || !IsValidUtf8(path)) {
		return false;
	}
	std::size_t begin = 0;
	while (begin < path.size()) {
		const auto end = path.find('/', begin);
		const auto length = (end == std::string::npos ? path.size() : end) - begin;
		if (length == 0 || (length == 1 && path[begin] == '.') ||
		    (length == 2 && path[begin] == '.' && path[begin + 1] == '.')) {
			return false;
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return true;
}

std::string AsciiFold(const std::string &value) {
	std::string folded(value);
	for (auto &character : folded) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte >= 'A' && byte <= 'Z') {
			character = static_cast<char>(byte - 'A' + 'a');
		}
	}
	return folded;
}

void AppendU64BigEndian(std::string &destination, std::uint64_t value) {
	for (int shift = 56; shift >= 0; shift -= 8) {
		destination.push_back(static_cast<char>((value >> shift) & 0xffU));
	}
}

} // namespace

std::string ComputeDigest(const std::string &framed) {
	return "sha256." + ComputeSha256Hex(framed);
}

bool ComputePackageDigest(const std::vector<SemanticSourceFile> &semantic_files, PackageCancellation &cancellation,
                          std::string &digest) {
	std::vector<const SemanticSourceFile *> ordered;
	std::set<std::string> folded_paths;
	ordered.reserve(semantic_files.size());
	for (const auto &file : semantic_files) {
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
		if (!IsNormalizedRelativePath(file.path)) {
			throw std::invalid_argument("semantic source path is not normalized relative POSIX UTF-8");
		}
		if (file.path.size() > std::numeric_limits<std::uint64_t>::max() ||
		    file.bytes.size() > std::numeric_limits<std::uint64_t>::max()) {
			throw std::invalid_argument("semantic source cannot be represented by the digest framing");
		}
		if (!folded_paths.insert(AsciiFold(file.path)).second) {
			throw std::invalid_argument("semantic source paths are duplicate or case-colliding");
		}
		ordered.push_back(&file);
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
	}
	std::sort(ordered.begin(), ordered.end(),
	          [](const SemanticSourceFile *left, const SemanticSourceFile *right) { return left->path < right->path; });

	std::string framed;
	std::size_t framed_size = 0;
	for (const auto *file : ordered) {
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
		const auto maximum = std::numeric_limits<std::size_t>::max();
		if (file->path.size() > maximum - 16U) {
			throw std::length_error("semantic source digest framing exceeds addressable memory");
		}
		const auto prefix_and_path = 16U + file->path.size();
		if (file->bytes.size() > maximum - prefix_and_path) {
			throw std::length_error("semantic source digest framing exceeds addressable memory");
		}
		const auto framed_file_size = prefix_and_path + file->bytes.size();
		if (framed_size > maximum - framed_file_size) {
			throw std::length_error("semantic source digest framing exceeds addressable memory");
		}
		framed_size += framed_file_size;
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
	}
	framed.reserve(framed_size);
	for (const auto *file : ordered) {
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
		AppendU64BigEndian(framed, static_cast<std::uint64_t>(file->path.size()));
		framed.append(file->path);
		AppendU64BigEndian(framed, static_cast<std::uint64_t>(file->bytes.size()));
		framed.append(file->bytes);
		if (cancellation.IsCancellationRequested()) {
			return false;
		}
	}
	if (cancellation.IsCancellationRequested()) {
		return false;
	}
	const auto computed = ComputeDigest(framed);
	if (cancellation.IsCancellationRequested()) {
		return false;
	}
	digest = computed;
	return true;
}

} // namespace connector
} // namespace cuac
