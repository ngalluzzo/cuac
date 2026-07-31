#include "cuac/runtime/generation_registry.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {
namespace {

const char *FailureMessage(RuntimeGenerationFailure failure) noexcept {
	switch (failure) {
	case RuntimeGenerationFailure::REGISTRY_CLOSING:
		return "Runtime generation registry is closing";
	case RuntimeGenerationFailure::STALE_BASE:
		return "Runtime generation staging base is stale";
	case RuntimeGenerationFailure::INVALID_LOCAL_PACKAGE:
		return "Runtime generation candidate is not a valid compiled local package";
	case RuntimeGenerationFailure::CONNECTOR_ALREADY_ACTIVE:
		return "Runtime generation connector is already active";
	case RuntimeGenerationFailure::CONNECTOR_NOT_ACTIVE:
		return "Runtime generation connector is not active";
	case RuntimeGenerationFailure::RELOAD_DECISION_MISMATCH:
		return "Runtime generation reload decision does not match the staged pair";
	case RuntimeGenerationFailure::RELOAD_REJECTED:
		return "Runtime generation reload was rejected";
	}
	return "Runtime generation registry rejected an unknown failure";
}

const char *RejectedReloadMessage(PackageReloadClassification rejection) noexcept {
	switch (rejection) {
	case PackageReloadClassification::REJECTED_PACKAGE_IDENTITY:
		return "Package reload violates immutable package identity";
	case PackageReloadClassification::INCOMPATIBLE_RELOAD:
		return "Package reload is incompatible with the active generation";
	case PackageReloadClassification::EXACT_NO_OP:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR:
	case PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR:
		return FailureMessage(RuntimeGenerationFailure::RELOAD_REJECTED);
	}
	return FailureMessage(RuntimeGenerationFailure::RELOAD_REJECTED);
}

const char *RejectedReloadCode(PackageReloadClassification rejection) noexcept {
	switch (rejection) {
	case PackageReloadClassification::REJECTED_PACKAGE_IDENTITY:
		return "CUAC_PACKAGE_IDENTITY";
	case PackageReloadClassification::INCOMPATIBLE_RELOAD:
		return "CUAC_INCOMPATIBLE_RELOAD";
	case PackageReloadClassification::EXACT_NO_OP:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_PATCH:
	case PackageReloadClassification::COMPATIBLE_PROVENANCE_MINOR:
	case PackageReloadClassification::COMPATIBLE_APPEND_ONLY_MINOR:
		return "";
	}
	return "";
}

} // namespace

RuntimeGenerationError::RuntimeGenerationError(RuntimeGenerationFailure failure_p) noexcept
    : failure(failure_p), rejection(PackageReloadClassification::EXACT_NO_OP) {
}

RuntimeGenerationError::RuntimeGenerationError(RuntimeGenerationFailure failure_p,
                                               PackageReloadClassification rejection_p) noexcept
    : failure(failure_p), rejection(rejection_p) {
}

const char *RuntimeGenerationError::what() const noexcept {
	if (failure == RuntimeGenerationFailure::RELOAD_REJECTED) {
		return RejectedReloadMessage(rejection);
	}
	return FailureMessage(failure);
}

RuntimeGenerationFailure RuntimeGenerationError::Failure() const noexcept {
	return failure;
}

bool RuntimeGenerationError::HasConnectorDiagnostic() const noexcept {
	return failure == RuntimeGenerationFailure::RELOAD_REJECTED && RejectedReloadCode(rejection)[0] != '\0';
}

const char *RuntimeGenerationError::DiagnosticCode() const noexcept {
	return HasConnectorDiagnostic() ? RejectedReloadCode(rejection) : "";
}

const char *RuntimeGenerationError::DiagnosticPhase() const noexcept {
	return HasConnectorDiagnostic() ? "compatibility" : "";
}

RuntimeGenerationOwner::RuntimeGenerationOwner(CompiledLocalPackage package_p) : package(std::move(package_p)) {
	if (!package.IsValid()) {
		throw std::invalid_argument("Runtime generation owner requires a valid compiled local package");
	}
}

const CompiledPackageGeneration &RuntimeGenerationOwner::Generation() const noexcept {
	return package.Generation();
}

const CompiledLocalPackage &RuntimeGenerationOwner::LocalPackage() const noexcept {
	return package;
}

RuntimeGenerationSnapshot::RuntimeGenerationSnapshot(
    std::vector<std::shared_ptr<const RuntimeGenerationOwner>> generations_p)
    : generations(std::move(generations_p)) {
}

const std::vector<std::shared_ptr<const RuntimeGenerationOwner>> &
RuntimeGenerationSnapshot::Generations() const noexcept {
	return generations;
}

std::shared_ptr<const RuntimeGenerationOwner> RuntimeGenerationSnapshot::Find(const std::string &connector_id) const {
	for (const auto &owner : generations) {
		if (owner->Generation().Identity().ConnectorId() == connector_id) {
			return owner;
		}
	}
	return nullptr;
}

} // namespace cuac
