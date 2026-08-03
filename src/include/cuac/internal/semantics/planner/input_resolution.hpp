#pragma once

#include "cuac/connector/catalog.hpp"
#include "cuac/query/scan_request.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace input_resolution {

// Relation inputs are resolved before operation eligibility. Omission remains
// distinct from explicit NULL, and the source records whether a concrete value
// came from Query or from Connector's compiled default. These values are
// planning facts only: they grant no request-encoding or Runtime authority.
enum class ResolvedInputState { UNBOUND, BOUND_NULL, BOUND_VALUE };
enum class ResolvedInputSource { NONE, EXPLICIT, DEFAULT_VALUE };

class ResolvedRelationInput {
public:
	ResolvedRelationInput(std::string name, CompiledScalarType type, ResolvedInputState state,
	                      ResolvedInputSource source, bool boolean_value, std::int64_t bigint_value,
	                      std::string varchar_value, double double_value, std::int64_t timestamptz_microseconds = 0);
	ResolvedRelationInput(const ResolvedRelationInput &) = default;
	ResolvedRelationInput(ResolvedRelationInput &&) = default;
	ResolvedRelationInput &operator=(const ResolvedRelationInput &) = delete;
	ResolvedRelationInput &operator=(ResolvedRelationInput &&) = delete;

	const std::string &Name() const noexcept;
	CompiledScalarType Type() const noexcept;
	ResolvedInputState State() const noexcept;
	ResolvedInputSource Source() const noexcept;
	bool BooleanValue() const;
	std::int64_t BigintValue() const;
	const std::string &VarcharValue() const;
	double DoubleValue() const;
	std::int64_t TimestamptzMicroseconds() const;

private:
	std::string name;
	CompiledScalarType type;
	ResolvedInputState state;
	ResolvedInputSource source;
	bool boolean_value;
	std::int64_t bigint_value;
	std::string varchar_value;
	double double_value;
	std::int64_t timestamptz_microseconds;
};

// Immutable exact-identifier lookup over all declared relation inputs. The
// collection retains declaration order so planning and diagnostics are stable.
class ResolvedRelationInputs {
public:
	ResolvedRelationInputs(const ResolvedRelationInputs &) = default;
	ResolvedRelationInputs(ResolvedRelationInputs &&) = default;
	ResolvedRelationInputs &operator=(const ResolvedRelationInputs &) = delete;
	ResolvedRelationInputs &operator=(ResolvedRelationInputs &&) = delete;

	std::size_t Size() const noexcept;
	const ResolvedRelationInput &At(std::size_t index) const;
	const ResolvedRelationInput *Find(const std::string &exact_identifier) const noexcept;

private:
	friend ResolvedRelationInputs ResolveRelationInputs(const CompiledRelation &, const ExplicitInputs &);

	explicit ResolvedRelationInputs(std::vector<ResolvedRelationInput> values);

	std::vector<ResolvedRelationInput> values;
};

// Applies Query's explicit values and Connector's typed defaults exactly once.
// Unknown inputs, type mismatches, and invalid NULLs fail with PlanningError;
// no operation is inspected and no I/O is performed.
ResolvedRelationInputs ResolveRelationInputs(const CompiledRelation &relation, const ExplicitInputs &explicit_inputs);

} // namespace input_resolution
} // namespace cuac
