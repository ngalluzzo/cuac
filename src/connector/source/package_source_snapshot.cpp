#include "cuac/internal/connector/source/package_source.hpp"

#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace cuac {
namespace connector {

namespace {

class FileDescriptorGuard {
public:
	explicit FileDescriptorGuard(int value_p) noexcept : value(value_p) {
	}

	~FileDescriptorGuard() noexcept {
		if (value >= 0) {
			::close(value);
		}
	}

	void Release() noexcept {
		value = -1;
	}

private:
	int value;
};

} // namespace

// This state/lifecycle unit is deliberately independent of source acquisition,
// YAML, and the compiler. Runtime can retain Connector's opaque local-package
// custody through its bounded provider target without linking those services.
class PackageSourceSnapshot::State {
public:
	State(int root_fd_p, std::vector<SemanticSourceFile> files_p, std::vector<std::string> relation_ids_p,
	      std::string digest_p)
	    : root_fd(root_fd_p), files(std::move(files_p)), relation_ids(std::move(relation_ids_p)),
	      digest(std::move(digest_p)) {
	}

	~State() noexcept {
		if (root_fd >= 0) {
			::close(root_fd);
		}
	}

	State(const State &) = delete;
	State &operator=(const State &) = delete;

	int root_fd;
	std::vector<SemanticSourceFile> files;
	std::vector<std::string> relation_ids;
	std::string digest;
};

PackageSourceSnapshot::PackageSourceSnapshot() {
}

PackageSourceSnapshot::PackageSourceSnapshot(std::shared_ptr<const State> state_p) : state(std::move(state_p)) {
}

PackageSourceSnapshot PackageSourceSnapshot::Create(int root_fd, std::vector<SemanticSourceFile> files,
                                                    std::vector<std::string> relation_ids, std::string digest) {
	// The guard owns the descriptor until State construction succeeds. State is
	// then the sole owner while unique_ptr transfers into shared_ptr, so a State
	// allocation, argument move, or shared control-block failure closes exactly
	// once without a raw-descriptor catch path.
	FileDescriptorGuard descriptor(root_fd);
	std::unique_ptr<State> owned(new State(root_fd, std::move(files), std::move(relation_ids), std::move(digest)));
	descriptor.Release();
	return PackageSourceSnapshot(std::shared_ptr<const State>(std::move(owned)));
}

const std::vector<SemanticSourceFile> &PackageSourceSnapshot::Files() const {
	if (!state) {
		throw std::logic_error("package source snapshot is empty");
	}
	return state->files;
}

const std::vector<std::string> &PackageSourceSnapshot::RelationIds() const {
	if (!state) {
		throw std::logic_error("package source snapshot is empty");
	}
	return state->relation_ids;
}

const std::string &PackageSourceSnapshot::Digest() const {
	if (!state) {
		throw std::logic_error("package source snapshot is empty");
	}
	return state->digest;
}

bool PackageSourceSnapshot::IsValid() const noexcept {
	return static_cast<bool>(state);
}

int PackageSourceSnapshot::RetainedRootFd() const {
	if (!state) {
		throw std::logic_error("package source snapshot is empty");
	}
	return state->root_fd;
}

} // namespace connector
} // namespace cuac
