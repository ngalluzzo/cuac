#include "cuac/internal/connector/model/operation_selector_declaration.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsSelectorInputIdentifier(const std::string &value) {
	if (value.empty() || value.size() > 63 || value.front() < 'a' || value.front() > 'z') {
		return false;
	}
	for (const auto character : value) {
		const bool lower = character >= 'a' && character <= 'z';
		const bool digit = character >= '0' && character <= '9';
		if (!lower && !digit && character != '_') {
			return false;
		}
	}
	return true;
}

void ValidateReferenceKind(CompiledRequiredInputKind kind) {
	switch (kind) {
	case CompiledRequiredInputKind::RELATION_INPUT:
	case CompiledRequiredInputKind::CONDITIONAL_INPUT:
		return;
	}
	throw std::invalid_argument("compiled required-input reference contains an unknown namespace");
}

bool ReferenceLess(const CompiledRequiredInputReference &left, const CompiledRequiredInputReference &right) {
	if (left.Kind() != right.Kind()) {
		return static_cast<int>(left.Kind()) < static_cast<int>(right.Kind());
	}
	return left.Id() < right.Id();
}

std::vector<CompiledRequiredInputReference>
NormalizeRequiredReferences(const std::vector<CompiledRequiredInputReference> &references) {
	if (references.size() > 128) {
		throw std::invalid_argument("compiled v1 operation selector exceeds the required-input limit");
	}
	std::vector<CompiledRequiredInputReference> normalized;
	normalized.reserve(references.size());
	std::vector<bool> emitted(references.size(), false);
	for (std::size_t output = 0; output < references.size(); output++) {
		std::size_t selected = references.size();
		for (std::size_t index = 0; index < references.size(); index++) {
			if (!emitted[index] &&
			    (selected == references.size() || ReferenceLess(references[index], references[selected]))) {
				selected = index;
			}
		}
		if (!normalized.empty() && normalized.back().Kind() == references[selected].Kind() &&
		    normalized.back().Id() == references[selected].Id()) {
			throw std::invalid_argument("compiled operation selector contains a duplicate tagged required input");
		}
		normalized.push_back(references[selected]);
		emitted[selected] = true;
	}
	return normalized;
}

bool HasRelationInput(const std::vector<CompiledRelationInput> &relation_inputs, const std::string &input) {
	for (const auto &relation_input : relation_inputs) {
		if (relation_input.Name() == input) {
			return true;
		}
	}
	return false;
}

bool HasConditionalInput(const CompiledOperation &operation, const std::vector<CompiledPredicateMapping> &mappings,
                         const std::string &input) {
	for (const auto &mapping : mappings) {
		if (mapping.OperationName() == operation.name && mapping.RemoteInputName() == input) {
			return true;
		}
	}
	return false;
}

} // namespace

CompiledRequiredInputReference::CompiledRequiredInputReference(CompiledRequiredInputKind kind_p, std::string id_p)
    : kind(kind_p), id(std::move(id_p)) {
	ValidateReferenceKind(kind);
	if (!IsSelectorInputIdentifier(id)) {
		throw std::invalid_argument("compiled required-input reference contains an invalid identifier");
	}
}

CompiledRequiredInputKind CompiledRequiredInputReference::Kind() const {
	return kind;
}

const std::string &CompiledRequiredInputReference::Id() const {
	return id;
}

CompiledOperationSelector::CompiledOperationSelector() {
}

CompiledOperationSelector::CompiledOperationSelector(
    std::vector<CompiledRequiredInputReference> required_input_references_p)
    : required_input_references(NormalizeRequiredReferences(required_input_references_p)) {
}

const std::vector<CompiledRequiredInputReference> &CompiledOperationSelector::RequiredInputReferences() const {
	return required_input_references;
}

namespace internal {

void ValidateOperationSelectorReferences(const CompiledOperation &operation,
                                         const std::vector<CompiledRelationInput> &relation_inputs,
                                         const std::vector<CompiledPredicateMapping> &mappings) {
	const auto &references = operation.selector.RequiredInputReferences();
	if (references.size() > 128 || operation.fallback != references.empty()) {
		throw std::invalid_argument("compiled v1 operation selector disagrees with fallback/when shape");
	}
	for (const auto &reference : references) {
		const bool represented = reference.Kind() == CompiledRequiredInputKind::RELATION_INPUT
		                             ? HasRelationInput(relation_inputs, reference.Id())
		                             : HasConditionalInput(operation, mappings, reference.Id());
		if (!represented) {
			throw std::invalid_argument("compiled operation selector references a missing or wrong-kind input");
		}
	}
}

} // namespace internal
} // namespace cuac
