#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include "cuac/internal/connector/source/failsafe_yaml_internal.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {

class FailsafeYamlNode::Data {
public:
	struct MappingEntry {
		std::string key;
		SourceSpan key_span;
		FailsafeYamlNode value;
	};

	Data(Kind kind_p, SourceSpan span_p) : kind(kind_p), span(span_p), scalar_style(ScalarStyle::PLAIN) {
	}

	Kind kind;
	SourceSpan span;
	std::string scalar;
	ScalarStyle scalar_style;
	std::vector<MappingEntry> mapping;
	std::vector<FailsafeYamlNode> sequence;
};

void FailsafeYamlParserAccess::ConsumeNode(FailsafeYamlBudget &budget, const std::string &file,
                                           const SourceSpan &span) {
	if (budget.nodes_consumed >= budget.limits.max_nodes) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, file, span, "YAML node budget is exhausted");
	}
	budget.nodes_consumed++;
}

void FailsafeYamlParserAccess::ConsumeKey(FailsafeYamlBudget &budget, const std::string &file, const SourceSpan &span) {
	ConsumeNode(budget, file, span);
}

FailsafeYamlNode FailsafeYamlParserAccess::Scalar(std::string value, SourceSpan span,
                                                  FailsafeYamlNode::ScalarStyle style, FailsafeYamlBudget &budget,
                                                  const std::string &file) {
	ConsumeNode(budget, file, span);
	auto data = std::make_shared<FailsafeYamlNode::Data>(FailsafeYamlNode::Kind::SCALAR, span);
	data->scalar = std::move(value);
	data->scalar_style = style;
	return FailsafeYamlNode(std::move(data));
}

FailsafeYamlNode FailsafeYamlParserAccess::Mapping(std::vector<MappingItem> entries, SourceSpan span) {
	auto data = std::make_shared<FailsafeYamlNode::Data>(FailsafeYamlNode::Kind::MAPPING, span);
	data->mapping.reserve(entries.size());
	for (auto &entry : entries) {
		FailsafeYamlNode::Data::MappingEntry retained;
		retained.key = std::move(entry.key);
		retained.key_span = entry.key_span;
		retained.value = std::move(entry.value);
		data->mapping.push_back(std::move(retained));
	}
	return FailsafeYamlNode(std::move(data));
}

FailsafeYamlNode FailsafeYamlParserAccess::Sequence(std::vector<FailsafeYamlNode> values, SourceSpan span) {
	auto data = std::make_shared<FailsafeYamlNode::Data>(FailsafeYamlNode::Kind::SEQUENCE, span);
	data->sequence = std::move(values);
	return FailsafeYamlNode(std::move(data));
}

FailsafeYamlError::FailsafeYamlError(FailsafeYamlErrorCode code_p, std::string file_p, SourceSpan span_p,
                                     std::string safe_message_p)
    : code(code_p), file(std::move(file_p)), span(span_p), safe_message(std::move(safe_message_p)) {
}

const char *FailsafeYamlError::what() const noexcept {
	return safe_message.c_str();
}

FailsafeYamlErrorCode FailsafeYamlError::Code() const noexcept {
	return code;
}

const std::string &FailsafeYamlError::File() const noexcept {
	return file;
}

const SourceSpan &FailsafeYamlError::Span() const noexcept {
	return span;
}

FailsafeYamlLimits FailsafeYamlLimits::V1() {
	return {32, 100000, 1024 * 1024, 4096};
}

FailsafeYamlBudget::FailsafeYamlBudget(FailsafeYamlLimits limits_p) : limits(limits_p), nodes_consumed(0) {
	if (limits.max_depth == 0 || limits.max_nodes == 0 || limits.max_scalar_bytes == 0 ||
	    limits.max_container_entries == 0) {
		throw std::invalid_argument("YAML limits must be positive");
	}
	const auto spec = FailsafeYamlLimits::V1();
	limits.max_depth = std::min(limits.max_depth, spec.max_depth);
	limits.max_nodes = std::min(limits.max_nodes, spec.max_nodes);
	limits.max_scalar_bytes = std::min(limits.max_scalar_bytes, spec.max_scalar_bytes);
	limits.max_container_entries = std::min(limits.max_container_entries, spec.max_container_entries);
}

const FailsafeYamlLimits &FailsafeYamlBudget::Limits() const noexcept {
	return limits;
}

std::uint64_t FailsafeYamlBudget::NodesConsumed() const noexcept {
	return nodes_consumed;
}

FailsafeYamlNode::FailsafeYamlNode() {
}

FailsafeYamlNode::FailsafeYamlNode(std::shared_ptr<const Data> data_p) : data(std::move(data_p)) {
}

FailsafeYamlNode::Kind FailsafeYamlNode::Type() const {
	if (!data) {
		throw std::logic_error("YAML node is empty");
	}
	return data->kind;
}

const SourceSpan &FailsafeYamlNode::Span() const {
	if (!data) {
		throw std::logic_error("YAML node is empty");
	}
	return data->span;
}

const std::string &FailsafeYamlNode::Scalar() const {
	if (!data || data->kind != Kind::SCALAR) {
		throw std::logic_error("YAML node is not a scalar");
	}
	return data->scalar;
}

FailsafeYamlNode::ScalarStyle FailsafeYamlNode::Style() const {
	if (!data || data->kind != Kind::SCALAR) {
		throw std::logic_error("YAML node is not a scalar");
	}
	return data->scalar_style;
}

std::size_t FailsafeYamlNode::Size() const {
	if (!data) {
		throw std::logic_error("YAML node is empty");
	}
	if (data->kind == Kind::MAPPING) {
		return data->mapping.size();
	}
	if (data->kind == Kind::SEQUENCE) {
		return data->sequence.size();
	}
	throw std::logic_error("YAML scalar has no entries");
}

const std::string &FailsafeYamlNode::MappingKey(std::size_t index) const {
	if (!data || data->kind != Kind::MAPPING) {
		throw std::logic_error("YAML node is not a mapping");
	}
	return data->mapping.at(index).key;
}

const SourceSpan &FailsafeYamlNode::MappingKeySpan(std::size_t index) const {
	if (!data || data->kind != Kind::MAPPING) {
		throw std::logic_error("YAML node is not a mapping");
	}
	return data->mapping.at(index).key_span;
}

const FailsafeYamlNode &FailsafeYamlNode::MappingValue(std::size_t index) const {
	if (!data || data->kind != Kind::MAPPING) {
		throw std::logic_error("YAML node is not a mapping");
	}
	return data->mapping.at(index).value;
}

const FailsafeYamlNode &FailsafeYamlNode::SequenceValue(std::size_t index) const {
	if (!data || data->kind != Kind::SEQUENCE) {
		throw std::logic_error("YAML node is not a sequence");
	}
	return data->sequence.at(index);
}

const FailsafeYamlNode *FailsafeYamlNode::Find(const std::string &key) const {
	if (!data || data->kind != Kind::MAPPING) {
		throw std::logic_error("YAML node is not a mapping");
	}
	for (const auto &entry : data->mapping) {
		if (entry.key == key) {
			return &entry.value;
		}
	}
	return nullptr;
}

FailsafeYamlNode ParseFailsafeYaml(const std::string &package_relative_file, const std::string &bytes,
                                   FailsafeYamlBudget &budget, PackageCancellation &cancellation) {
	auto lines = internal::LexFailsafeYaml(package_relative_file, bytes, cancellation);
	return internal::ParseFailsafeYamlStructure(package_relative_file, bytes.size(), std::move(lines), budget,
	                                            cancellation);
}

} // namespace connector
} // namespace cuac
