#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace cuac {

static const std::size_t MAX_REQUESTED_PREDICATE_DEPTH = 16;
static const std::size_t MAX_REQUESTED_PREDICATE_NODES = 64;

enum class RequestedPredicateKind { UNRESTRICTED, COMPARISON, CONJUNCTION, DISJUNCTION, NEGATION, UNSUPPORTED };
enum class RequestedPredicateComparisonOperator { EQUALS };
enum class RequestedPredicateValueKind { BIGINT, VARCHAR, BOOLEAN, DOUBLE };

// DuckDB's retained local predicate scope. This is supplied by Query beside
// the candidate tree and remains the authoritative description of which
// predicate DuckDB will still evaluate after any remote narrowing.
enum class RetainedPredicateScope { UNRESTRICTED, REQUESTED_PREDICATE, COMPLETE_DUCKDB_FILTER };

// Typed scalar identity carried by a comparison leaf. It contains no DuckDB
// object, coercion rule, request encoding, or execution authority. NULL and
// values that Query cannot identify without coercion are represented by an
// opaque unsupported candidate instead.
class RequestedPredicateValue {
public:
	static RequestedPredicateValue BigInt(std::int64_t value);
	static RequestedPredicateValue Varchar(std::string value);
	static RequestedPredicateValue Boolean(bool value);
	static RequestedPredicateValue Double(double value);

	RequestedPredicateValueKind Kind() const noexcept;
	std::int64_t BigIntValue() const;
	const std::string &VarcharValue() const;
	bool BooleanValue() const;
	double DoubleValue() const;

	bool operator==(const RequestedPredicateValue &other) const noexcept;
	bool operator!=(const RequestedPredicateValue &other) const noexcept;
	std::string Snapshot() const;

private:
	explicit RequestedPredicateValue(std::int64_t value);
	explicit RequestedPredicateValue(std::string value);
	explicit RequestedPredicateValue(bool value);
	explicit RequestedPredicateValue(double value);

	RequestedPredicateValueKind kind;
	std::int64_t bigint_value;
	std::string varchar_value;
	bool boolean_value;
	double double_value;
};

class RequestedPredicateNode;

// Bounded protocol-neutral relational structure shared by Query and
// Relational Semantics. Query constructs the complete structure DuckDB offers;
// Semantics alone matches Connector mappings and assigns remote meaning. The
// immutable tree is safe to retain across prepared and concurrent bind copies.
// It contains no SQL text, DuckDB object, function body, collation inference,
// remote input, credential, or I/O authority.
class RequestedPredicate {
public:
	// Conservative default used when no structured restriction is available.
	RequestedPredicate();

	static RequestedPredicate Unrestricted();
	static RequestedPredicate Comparison(std::size_t bound_column_index, RequestedPredicateValueKind bound_column_type,
	                                     RequestedPredicateComparisonOperator comparison_operator,
	                                     RequestedPredicateValue literal);
	static RequestedPredicate Conjunction(std::vector<RequestedPredicate> children);
	static RequestedPredicate Disjunction(std::vector<RequestedPredicate> children);
	static RequestedPredicate Negation(RequestedPredicate child);
	static RequestedPredicate Unsupported(std::uint64_t position);

	RequestedPredicateKind Kind() const noexcept;
	std::size_t BoundColumnIndex() const;
	RequestedPredicateValueKind BoundColumnType() const;
	RequestedPredicateComparisonOperator ComparisonOperator() const;
	const RequestedPredicateValue &Literal() const;
	const std::vector<RequestedPredicate> &Children() const;
	std::uint64_t UnsupportedPosition() const;

	std::size_t Depth() const noexcept;
	std::size_t NodeCount() const noexcept;
	bool StructurallyEquals(const RequestedPredicate &other) const noexcept;
	bool operator==(const RequestedPredicate &other) const noexcept;
	bool operator!=(const RequestedPredicate &other) const noexcept;

	// Stable structural explanation. String values are hex escaped. The output
	// is not SQL, serialization, or authority for Connector or Runtime behavior.
	std::string Snapshot() const;

private:
	explicit RequestedPredicate(std::shared_ptr<const RequestedPredicateNode> node);
	static RequestedPredicate BooleanNode(RequestedPredicateKind kind, std::vector<RequestedPredicate> children);

	std::shared_ptr<const RequestedPredicateNode> node;
};

} // namespace cuac
