#pragma once

#include <string>
#include <vector>

namespace cuac {
namespace connector {

class PackageCancellation;

struct SemanticSourceFile {
	std::string path;
	std::string bytes;
};

// Computes RFC 0013's sha256-length-prefixed-path-and-bytes-v1 identity. Paths
// are validated relative POSIX UTF-8 names, sorted bytewise, and framed as
// u64be(path length), path bytes, u64be(content length), raw content. Metadata,
// absolute roots, README, fixtures, and YAML normalization never enter the
// digest. Cancellation is checked before and after every bounded file-framing
// unit and the final bounded hash unit. A cancelled call returns false without
// changing `digest`; invalid or duplicate paths throw std::invalid_argument.
bool ComputePackageDigest(const std::vector<SemanticSourceFile> &semantic_files, PackageCancellation &cancellation,
                          std::string &digest);

} // namespace connector
} // namespace cuac
