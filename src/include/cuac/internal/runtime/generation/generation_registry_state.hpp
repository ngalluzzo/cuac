#pragma once

#include "cuac/runtime/generation_registry.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

namespace cuac {
namespace internal {

// Private synchronization state shared with outstanding leases so registry
// object destruction cannot invalidate an in-flight terminal callback.
struct RuntimeGenerationRegistryState final {
	explicit RuntimeGenerationRegistryState(std::shared_ptr<const RuntimeGenerationSnapshot> active_p)
	    : closing(false), waiting_stages(0), active(std::move(active_p)) {
	}

	std::timed_mutex publication_mutex;
	std::atomic<bool> closing;
	std::atomic<std::uint64_t> waiting_stages;
	std::shared_ptr<const RuntimeGenerationSnapshot> active;
};

// A publication owns the Runtime serializer from successful staging through
// Query's eventual commit or rollback callback. `target` is fully allocated
// before construction, leaving only pointer swap and unlock at commit.
struct RuntimeGenerationPublicationState final {
	RuntimeGenerationPublicationState(std::shared_ptr<RuntimeGenerationRegistryState> registry_p,
	                                  std::unique_lock<std::timed_mutex> publication_lock_p,
	                                  std::shared_ptr<const RuntimeGenerationSnapshot> target_p)
	    : registry(std::move(registry_p)), publication_lock(std::move(publication_lock_p)), target(std::move(target_p)),
	      pending(true) {
	}

	std::shared_ptr<RuntimeGenerationRegistryState> registry;
	std::unique_lock<std::timed_mutex> publication_lock;
	std::shared_ptr<const RuntimeGenerationSnapshot> target;
	bool pending;
};

} // namespace internal
} // namespace cuac
