#pragma once

#include "cuac/connector/catalog.hpp"

#include <memory>
#include <string>
#include <vector>

namespace cuac {

namespace internal {
class CompiledModelBuilder;
struct CompiledPackageGenerationState;
} // namespace internal

// Stable identity of one package-compiled generation. These values are
// provenance and compatibility facts, never request, credential, network, or
// catalog authority. Construction is restricted to Connector's validated
// compiler boundary.
class CompiledPackageIdentity {
public:
	CompiledPackageIdentity(const CompiledPackageIdentity &) = default;
	CompiledPackageIdentity(CompiledPackageIdentity &&) = default;
	CompiledPackageIdentity &operator=(const CompiledPackageIdentity &) = delete;
	CompiledPackageIdentity &operator=(CompiledPackageIdentity &&) = delete;

	const std::string &SpecIdentifier() const;
	const std::string &ConnectorId() const;
	const std::string &PackageVersion() const;
	const std::string &PackageDigest() const;

private:
	friend class internal::CompiledModelBuilder;

	CompiledPackageIdentity(std::string spec_identifier, std::string connector_id, std::string package_version,
	                        std::string package_digest);

	std::string spec_identifier;
	std::string connector_id;
	std::string package_version;
	std::string package_digest;
};

// Opaque shared ownership token for the exact immutable generation observed by
// Query. It deliberately exposes no Connector, operation, predicate, source,
// policy, or runtime state. Copying the handle pins lifetime across bind,
// prepare, execution, and reload without granting mutation.
class CompiledGenerationHandle {
public:
	CompiledGenerationHandle(const CompiledGenerationHandle &) = default;
	CompiledGenerationHandle(CompiledGenerationHandle &&) = default;
	CompiledGenerationHandle &operator=(const CompiledGenerationHandle &) = delete;
	CompiledGenerationHandle &operator=(CompiledGenerationHandle &&) = delete;

	bool IsValid() const {
		return static_cast<bool>(state);
	}
	bool IsSameGeneration(const CompiledGenerationHandle &other) const {
		return state == other.state;
	}

private:
	friend class CompiledPackageGeneration;
	friend class CacheSemanticIdentity;
	friend class ScanPlanBuilder;

	CompiledGenerationHandle() = default;
	explicit CompiledGenerationHandle(std::shared_ptr<const internal::CompiledPackageGenerationState> state);

	std::shared_ptr<const internal::CompiledPackageGenerationState> state;
};

// Query-visible output descriptor. Extractors and protocol facts remain behind
// Connector/Semantics interfaces; Query receives only the ordered DuckDB
// structural shape required for registration and bind.
class CompiledRegistrationColumn {
public:
	CompiledRegistrationColumn(const CompiledRegistrationColumn &) = default;
	CompiledRegistrationColumn(CompiledRegistrationColumn &&) = default;
	CompiledRegistrationColumn &operator=(const CompiledRegistrationColumn &) = delete;
	CompiledRegistrationColumn &operator=(CompiledRegistrationColumn &&) = delete;

	const std::string &Name() const;
	CompiledColumnShape Shape() const;
	CompiledScalarType Type() const;
	bool ElementNullable() const;
	bool Nullable() const;

private:
	friend class CompiledPackageGeneration;

	CompiledRegistrationColumn(std::string name, CompiledColumnShape shape, CompiledScalarType type,
	                           bool element_nullable, bool nullable);

	std::string name;
	CompiledColumnShape shape;
	CompiledScalarType type;
	bool element_nullable;
	bool nullable;
};

enum class CompiledRegistrationAuthentication { ANONYMOUS, LOGICAL_SECRET_REQUIRED };

// One relation projected for Query registration. Inputs preserve source order,
// nullable behavior, default presence, and typed NULL without exposing
// operation selection or transport declarations. Query synthesizes `secret`
// only from Authentication(); it never receives a credential identifier.
class CompiledRegistrationRelation {
public:
	CompiledRegistrationRelation(const CompiledRegistrationRelation &) = default;
	CompiledRegistrationRelation(CompiledRegistrationRelation &&) = default;
	CompiledRegistrationRelation &operator=(const CompiledRegistrationRelation &) = delete;
	CompiledRegistrationRelation &operator=(CompiledRegistrationRelation &&) = delete;

	const std::string &Name() const;
	const std::vector<CompiledRegistrationColumn> &Columns() const;
	const std::vector<CompiledRelationInput> &Inputs() const;
	CompiledRegistrationAuthentication Authentication() const;

private:
	friend class CompiledPackageGeneration;

	CompiledRegistrationRelation(std::string name, std::vector<CompiledRegistrationColumn> columns,
	                             std::vector<CompiledRelationInput> inputs,
	                             CompiledRegistrationAuthentication authentication);

	std::string name;
	std::vector<CompiledRegistrationColumn> columns;
	std::vector<CompiledRelationInput> inputs;
	CompiledRegistrationAuthentication authentication;
};

// Deliberately narrow Connector-to-Query service. It is a value projection
// backed by an opaque lifetime handle; no consumer needs package source, a
// CompiledConnector, or a provider-private constructor to publish functions.
class CompiledQueryRegistrationView {
public:
	CompiledQueryRegistrationView(const CompiledQueryRegistrationView &) = default;
	CompiledQueryRegistrationView(CompiledQueryRegistrationView &&) = default;
	CompiledQueryRegistrationView &operator=(const CompiledQueryRegistrationView &) = delete;
	CompiledQueryRegistrationView &operator=(CompiledQueryRegistrationView &&) = delete;

	const CompiledPackageIdentity &Identity() const;
	const std::vector<CompiledRegistrationRelation> &Relations() const;
	const CompiledGenerationHandle &GenerationHandle() const;

private:
	friend class CompiledPackageGeneration;

	CompiledQueryRegistrationView(CompiledPackageIdentity identity, std::vector<CompiledRegistrationRelation> relations,
	                              CompiledGenerationHandle generation_handle);

	CompiledPackageIdentity identity;
	std::vector<CompiledRegistrationRelation> relations;
	CompiledGenerationHandle generation_handle;
};

// Immutable Connector provider object for one package compilation. The shared
// state owns the generalized connector and survives as long as any generation,
// Query view, or opaque handle remains. Connector compilation constructs it
// only after complete validation; this type performs no I/O, publication,
// reload, cancellation, or execution work.
class CompiledPackageGeneration {
public:
	CompiledPackageGeneration(const CompiledPackageGeneration &) = default;
	CompiledPackageGeneration(CompiledPackageGeneration &&) = default;
	CompiledPackageGeneration &operator=(const CompiledPackageGeneration &) = delete;
	CompiledPackageGeneration &operator=(CompiledPackageGeneration &&) = delete;

	const CompiledPackageIdentity &Identity() const;
	const CompiledConnector &Connector() const;
	CompiledQueryRegistrationView QueryRegistration() const;
	CompiledGenerationHandle OpaqueHandle() const;

private:
	friend class internal::CompiledModelBuilder;

	explicit CompiledPackageGeneration(std::shared_ptr<const internal::CompiledPackageGenerationState> state);

	std::shared_ptr<const internal::CompiledPackageGenerationState> state;
};

} // namespace cuac
