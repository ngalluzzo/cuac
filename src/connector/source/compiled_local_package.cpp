#include "cuac/internal/connector/source/compiled_local_package.hpp"

#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace cuac {

class CompiledLocalPackage::State {
public:
	State(std::shared_ptr<const CompiledPackageGeneration> generation_p, connector::PackageSourceSnapshot source_p)
	    : generation(std::move(generation_p)), source(std::move(source_p)) {
		if (!generation || !source.IsValid()) {
			throw std::invalid_argument("compiled local package requires one generation and retained source root");
		}
	}

	std::shared_ptr<const CompiledPackageGeneration> generation;
	connector::PackageSourceSnapshot source;
};

CompiledLocalPackage::CompiledLocalPackage() noexcept {
}

CompiledLocalPackage::CompiledLocalPackage(std::shared_ptr<const State> state_p) : state(std::move(state_p)) {
	if (!state) {
		throw std::invalid_argument("compiled local package state cannot be empty");
	}
}

bool CompiledLocalPackage::IsValid() const noexcept {
	return static_cast<bool>(state);
}

const CompiledPackageGeneration &CompiledLocalPackage::Generation() const {
	if (!state) {
		throw std::logic_error("compiled local package is empty");
	}
	return *state->generation;
}

bool CompiledLocalPackage::MatchesGeneration(const CompiledGenerationHandle &generation) const noexcept {
	return state && state->generation->OpaqueHandle().IsSameGeneration(generation);
}

namespace internal {

CompiledLocalPackage CompiledLocalPackageAccess::Create(std::shared_ptr<const CompiledPackageGeneration> generation,
                                                        connector::PackageSourceSnapshot source) {
	std::unique_ptr<CompiledLocalPackage::State> owned(
	    new CompiledLocalPackage::State(std::move(generation), std::move(source)));
	return CompiledLocalPackage(std::shared_ptr<const CompiledLocalPackage::State>(std::move(owned)));
}

const connector::PackageSourceSnapshot &CompiledLocalPackageAccess::Source(const CompiledLocalPackage &package) {
	if (!package.state) {
		throw std::logic_error("compiled local package is empty");
	}
	return package.state->source;
}

int CompiledLocalPackageAccess::DuplicateRootForFixtures(const CompiledLocalPackage &package) {
	const auto &source = Source(package);
#if defined(F_DUPFD_CLOEXEC)
	const int duplicated = ::fcntl(source.RetainedRootFd(), F_DUPFD_CLOEXEC, 0);
#else
	const int duplicated = ::dup(source.RetainedRootFd());
	if (duplicated >= 0) {
		::fcntl(duplicated, F_SETFD, FD_CLOEXEC);
	}
#endif
	if (duplicated < 0) {
		throw std::runtime_error("retained package root is unavailable for fixture custody");
	}
	return duplicated;
}

} // namespace internal
} // namespace cuac
