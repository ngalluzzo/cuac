#include "cuac/semantics/relational_predicate.hpp"

#include <algorithm>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

const char *ValueKindName(RequestedPredicateValueKind kind) {
	switch (kind) {
	case RequestedPredicateValueKind::BIGINT:
		return "bigint";
	case RequestedPredicateValueKind::VARCHAR:
		return "varchar";
	case RequestedPredicateValueKind::BOOLEAN:
		return "boolean";
	case RequestedPredicateValueKind::DOUBLE:
		return "double";
	}
	throw std::logic_error("requested predicate contains an unknown value kind");
}

const char *ComparisonOperatorName(RequestedPredicateComparisonOperator comparison_operator) {
	switch (comparison_operator) {
	case RequestedPredicateComparisonOperator::EQUALS:
		return "equals";
	}
	throw std::logic_error("requested predicate contains an unknown comparison operator");
}

std::string HexEncode(const std::string &value) {
	static const char HEX_DIGITS[] = "0123456789abcdef";
	std::string result;
	result.reserve(value.size() * 2);
	for (const char character : value) {
		const auto byte = static_cast<unsigned char>(character);
		result.push_back(HEX_DIGITS[byte >> 4]);
		result.push_back(HEX_DIGITS[byte & 0x0f]);
	}
	return result;
}

} // namespace

RequestedPredicateValue::RequestedPredicateValue(std::int64_t value)
    : kind(RequestedPredicateValueKind::BIGINT), bigint_value(value), varchar_value(), boolean_value(false),
      double_value(0.0) {
}

RequestedPredicateValue::RequestedPredicateValue(std::string value)
    : kind(RequestedPredicateValueKind::VARCHAR), bigint_value(0), varchar_value(std::move(value)),
      boolean_value(false), double_value(0.0) {
}

RequestedPredicateValue::RequestedPredicateValue(bool value)
    : kind(RequestedPredicateValueKind::BOOLEAN), bigint_value(0), varchar_value(), boolean_value(value),
      double_value(0.0) {
}

RequestedPredicateValue::RequestedPredicateValue(double value)
    : kind(RequestedPredicateValueKind::DOUBLE), bigint_value(0), varchar_value(), boolean_value(false),
      // RFC 0020: -0.0 is normalized to 0.0 so every consumer sees one canonical zero.
      double_value(value == 0.0 ? 0.0 : value) {
}

RequestedPredicateValue RequestedPredicateValue::BigInt(std::int64_t value) {
	return RequestedPredicateValue(value);
}

RequestedPredicateValue RequestedPredicateValue::Varchar(std::string value) {
	return RequestedPredicateValue(std::move(value));
}

RequestedPredicateValue RequestedPredicateValue::Boolean(bool value) {
	return RequestedPredicateValue(value);
}

RequestedPredicateValue RequestedPredicateValue::Double(double value) {
	return RequestedPredicateValue(value);
}

RequestedPredicateValueKind RequestedPredicateValue::Kind() const noexcept {
	return kind;
}

std::int64_t RequestedPredicateValue::BigIntValue() const {
	if (kind != RequestedPredicateValueKind::BIGINT) {
		throw std::logic_error("requested predicate value is not BIGINT");
	}
	return bigint_value;
}

const std::string &RequestedPredicateValue::VarcharValue() const {
	if (kind != RequestedPredicateValueKind::VARCHAR) {
		throw std::logic_error("requested predicate value is not VARCHAR");
	}
	return varchar_value;
}

bool RequestedPredicateValue::BooleanValue() const {
	if (kind != RequestedPredicateValueKind::BOOLEAN) {
		throw std::logic_error("requested predicate value is not BOOLEAN");
	}
	return boolean_value;
}

double RequestedPredicateValue::DoubleValue() const {
	if (kind != RequestedPredicateValueKind::DOUBLE) {
		throw std::logic_error("requested predicate value is not DOUBLE");
	}
	return double_value;
}

bool RequestedPredicateValue::operator==(const RequestedPredicateValue &other) const noexcept {
	return kind == other.kind && bigint_value == other.bigint_value && varchar_value == other.varchar_value &&
	       boolean_value == other.boolean_value && double_value == other.double_value;
}

bool RequestedPredicateValue::operator!=(const RequestedPredicateValue &other) const noexcept {
	return !(*this == other);
}

std::string RequestedPredicateValue::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << ValueKindName(kind) << ':';
	switch (kind) {
	case RequestedPredicateValueKind::BIGINT:
		result << bigint_value;
		break;
	case RequestedPredicateValueKind::VARCHAR:
		result << "hex:" << HexEncode(varchar_value);
		break;
	case RequestedPredicateValueKind::BOOLEAN:
		result << (boolean_value ? "true" : "false");
		break;
	case RequestedPredicateValueKind::DOUBLE:
		result.precision(17);
		result << double_value;
		break;
	}
	return result.str();
}

class RequestedPredicateNode {
public:
	RequestedPredicateNode(RequestedPredicateKind kind_p, std::size_t bound_column_index_p,
	                       RequestedPredicateValueKind bound_column_type_p,
	                       RequestedPredicateComparisonOperator comparison_operator_p,
	                       RequestedPredicateValue literal_p, std::vector<RequestedPredicate> children_p,
	                       std::uint64_t unsupported_position_p, std::size_t depth_p, std::size_t node_count_p)
	    : kind(kind_p), bound_column_index(bound_column_index_p), bound_column_type(bound_column_type_p),
	      comparison_operator(comparison_operator_p), literal(std::move(literal_p)), children(std::move(children_p)),
	      unsupported_position(unsupported_position_p), depth(depth_p), node_count(node_count_p) {
	}

	RequestedPredicateKind kind;
	std::size_t bound_column_index;
	RequestedPredicateValueKind bound_column_type;
	RequestedPredicateComparisonOperator comparison_operator;
	RequestedPredicateValue literal;
	std::vector<RequestedPredicate> children;
	std::uint64_t unsupported_position;
	std::size_t depth;
	std::size_t node_count;
};

namespace {

std::shared_ptr<const RequestedPredicateNode> LeafNode(RequestedPredicateKind kind, RequestedPredicateValue literal,
                                                       std::uint64_t unsupported_position = 0) {
	return std::make_shared<const RequestedPredicateNode>(
	    kind, 0, RequestedPredicateValueKind::BIGINT, RequestedPredicateComparisonOperator::EQUALS, std::move(literal),
	    std::vector<RequestedPredicate>(), unsupported_position, 1, 1);
}

void RequireKnownValueKind(RequestedPredicateValueKind kind) {
	(void)ValueKindName(kind);
}

void RequireKnownComparisonOperator(RequestedPredicateComparisonOperator comparison_operator) {
	(void)ComparisonOperatorName(comparison_operator);
}

} // namespace

RequestedPredicate::RequestedPredicate()
    : node(LeafNode(RequestedPredicateKind::UNRESTRICTED, RequestedPredicateValue::BigInt(0))) {
}

RequestedPredicate::RequestedPredicate(std::shared_ptr<const RequestedPredicateNode> node_p) : node(std::move(node_p)) {
	if (!node) {
		throw std::invalid_argument("requested predicate node must not be null");
	}
}

RequestedPredicate RequestedPredicate::Unrestricted() {
	return RequestedPredicate();
}

RequestedPredicate RequestedPredicate::Comparison(std::size_t bound_column_index,
                                                  RequestedPredicateValueKind bound_column_type,
                                                  RequestedPredicateComparisonOperator comparison_operator,
                                                  RequestedPredicateValue literal) {
	RequireKnownValueKind(bound_column_type);
	RequireKnownComparisonOperator(comparison_operator);
	if (bound_column_type != literal.Kind()) {
		throw std::invalid_argument("requested predicate comparison column and literal types disagree");
	}
	return RequestedPredicate(std::make_shared<const RequestedPredicateNode>(
	    RequestedPredicateKind::COMPARISON, bound_column_index, bound_column_type, comparison_operator,
	    std::move(literal), std::vector<RequestedPredicate>(), 0, 1, 1));
}

RequestedPredicate RequestedPredicate::BooleanNode(RequestedPredicateKind kind,
                                                   std::vector<RequestedPredicate> children) {
	if ((kind != RequestedPredicateKind::CONJUNCTION && kind != RequestedPredicateKind::DISJUNCTION) ||
	    children.size() < 2) {
		throw std::invalid_argument("requested predicate Boolean node requires at least two children");
	}
	std::size_t depth = 1;
	std::size_t node_count = 1;
	for (const auto &child : children) {
		depth = std::max(depth, child.Depth() + 1);
		if (child.NodeCount() > MAX_REQUESTED_PREDICATE_NODES - node_count) {
			throw std::invalid_argument("requested predicate exceeds its node limit");
		}
		node_count += child.NodeCount();
	}
	if (depth > MAX_REQUESTED_PREDICATE_DEPTH || node_count > MAX_REQUESTED_PREDICATE_NODES) {
		throw std::invalid_argument("requested predicate exceeds its structural limits");
	}
	return RequestedPredicate(std::make_shared<const RequestedPredicateNode>(
	    kind, 0, RequestedPredicateValueKind::BIGINT, RequestedPredicateComparisonOperator::EQUALS,
	    RequestedPredicateValue::BigInt(0), std::move(children), 0, depth, node_count));
}

RequestedPredicate RequestedPredicate::Conjunction(std::vector<RequestedPredicate> children) {
	return BooleanNode(RequestedPredicateKind::CONJUNCTION, std::move(children));
}

RequestedPredicate RequestedPredicate::Disjunction(std::vector<RequestedPredicate> children) {
	return BooleanNode(RequestedPredicateKind::DISJUNCTION, std::move(children));
}

RequestedPredicate RequestedPredicate::Negation(RequestedPredicate child) {
	if (child.Depth() + 1 > MAX_REQUESTED_PREDICATE_DEPTH || child.NodeCount() + 1 > MAX_REQUESTED_PREDICATE_NODES) {
		throw std::invalid_argument("requested predicate negation exceeds its structural limits");
	}
	const auto depth = child.Depth() + 1;
	const auto node_count = child.NodeCount() + 1;
	std::vector<RequestedPredicate> children;
	children.push_back(std::move(child));
	return RequestedPredicate(std::make_shared<const RequestedPredicateNode>(
	    RequestedPredicateKind::NEGATION, 0, RequestedPredicateValueKind::BIGINT,
	    RequestedPredicateComparisonOperator::EQUALS, RequestedPredicateValue::BigInt(0), std::move(children), 0, depth,
	    node_count));
}

RequestedPredicate RequestedPredicate::Unsupported(std::uint64_t position) {
	return RequestedPredicate(
	    LeafNode(RequestedPredicateKind::UNSUPPORTED, RequestedPredicateValue::BigInt(0), position));
}

RequestedPredicateKind RequestedPredicate::Kind() const noexcept {
	return node->kind;
}

std::size_t RequestedPredicate::BoundColumnIndex() const {
	if (Kind() != RequestedPredicateKind::COMPARISON) {
		throw std::logic_error("requested predicate is not a comparison");
	}
	return node->bound_column_index;
}

RequestedPredicateValueKind RequestedPredicate::BoundColumnType() const {
	if (Kind() != RequestedPredicateKind::COMPARISON) {
		throw std::logic_error("requested predicate is not a comparison");
	}
	return node->bound_column_type;
}

RequestedPredicateComparisonOperator RequestedPredicate::ComparisonOperator() const {
	if (Kind() != RequestedPredicateKind::COMPARISON) {
		throw std::logic_error("requested predicate is not a comparison");
	}
	return node->comparison_operator;
}

const RequestedPredicateValue &RequestedPredicate::Literal() const {
	if (Kind() != RequestedPredicateKind::COMPARISON) {
		throw std::logic_error("requested predicate is not a comparison");
	}
	return node->literal;
}

const std::vector<RequestedPredicate> &RequestedPredicate::Children() const {
	if (Kind() != RequestedPredicateKind::CONJUNCTION && Kind() != RequestedPredicateKind::DISJUNCTION &&
	    Kind() != RequestedPredicateKind::NEGATION) {
		throw std::logic_error("requested predicate node has no children");
	}
	return node->children;
}

std::uint64_t RequestedPredicate::UnsupportedPosition() const {
	if (Kind() != RequestedPredicateKind::UNSUPPORTED) {
		throw std::logic_error("requested predicate is not an opaque unsupported leaf");
	}
	return node->unsupported_position;
}

std::size_t RequestedPredicate::Depth() const noexcept {
	return node->depth;
}

std::size_t RequestedPredicate::NodeCount() const noexcept {
	return node->node_count;
}

bool RequestedPredicate::StructurallyEquals(const RequestedPredicate &other) const noexcept {
	if (Kind() != other.Kind() || Depth() != other.Depth() || NodeCount() != other.NodeCount()) {
		return false;
	}
	switch (Kind()) {
	case RequestedPredicateKind::UNRESTRICTED:
		return true;
	case RequestedPredicateKind::COMPARISON:
		return node->bound_column_index == other.node->bound_column_index &&
		       node->bound_column_type == other.node->bound_column_type &&
		       node->comparison_operator == other.node->comparison_operator && node->literal == other.node->literal;
	case RequestedPredicateKind::UNSUPPORTED:
		return node->unsupported_position == other.node->unsupported_position;
	case RequestedPredicateKind::CONJUNCTION:
	case RequestedPredicateKind::DISJUNCTION:
	case RequestedPredicateKind::NEGATION:
		if (node->children.size() != other.node->children.size()) {
			return false;
		}
		for (std::size_t index = 0; index < node->children.size(); index++) {
			if (!node->children[index].StructurallyEquals(other.node->children[index])) {
				return false;
			}
		}
		return true;
	}
	return false;
}

bool RequestedPredicate::operator==(const RequestedPredicate &other) const noexcept {
	return StructurallyEquals(other);
}

bool RequestedPredicate::operator!=(const RequestedPredicate &other) const noexcept {
	return !(*this == other);
}

std::string RequestedPredicate::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	switch (Kind()) {
	case RequestedPredicateKind::UNRESTRICTED:
		return "true";
	case RequestedPredicateKind::COMPARISON:
		result << "comparison[column:" << node->bound_column_index << ",type:" << ValueKindName(node->bound_column_type)
		       << ",operator:" << ComparisonOperatorName(node->comparison_operator)
		       << ",literal:" << node->literal.Snapshot() << ']';
		return result.str();
	case RequestedPredicateKind::UNSUPPORTED:
		result << "unsupported[position:" << node->unsupported_position << ']';
		return result.str();
	case RequestedPredicateKind::CONJUNCTION:
		result << "and[";
		break;
	case RequestedPredicateKind::DISJUNCTION:
		result << "or[";
		break;
	case RequestedPredicateKind::NEGATION:
		result << "not[";
		break;
	}
	for (std::size_t index = 0; index < node->children.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << node->children[index].Snapshot();
	}
	result << ']';
	return result.str();
}

} // namespace cuac
