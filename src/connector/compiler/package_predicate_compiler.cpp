#include "cuac/internal/connector/compiler/package_model_compiler.hpp"

#include "cuac/internal/connector/model/predicate_declaration.hpp"

#include <set>
#include <stdexcept>

namespace cuac {
namespace connector {
namespace internal {

namespace {

const ColumnDeclaration *FindColumn(const RelationDeclaration &relation, const std::string &id) {
	for (const auto &column : relation.columns) {
		if (column.id.value == id) {
			return &column;
		}
	}
	return nullptr;
}

const CompiledOperation *FindOperation(const std::vector<CompiledOperation> &operations, const std::string &id) {
	for (const auto &operation : operations) {
		if (operation.name == id) {
			return &operation;
		}
	}
	return nullptr;
}

bool HasConditionalBinding(const CompiledOperation &operation, const std::string &id) {
	if (operation.Protocol() != CompiledProtocol::REST) {
		return false;
	}
	for (const auto &field : operation.Rest().request.query_parameters) {
		if (field.source == CompiledQueryValueSource::CONDITIONAL_INPUT && field.source_id == id) {
			return true;
		}
	}
	return false;
}

bool SameScalar(const CompiledScalarValue &left, const CompiledScalarValue &right) {
	if (left.Type() != right.Type()) {
		return false;
	}
	switch (left.Type()) {
	case CompiledScalarType::BOOLEAN:
		return left.Boolean() == right.Boolean();
	case CompiledScalarType::BIGINT:
		return left.Bigint() == right.Bigint();
	case CompiledScalarType::VARCHAR:
		return left.Varchar() == right.Varchar();
	case CompiledScalarType::DOUBLE:
		return left.Double() == right.Double();
	}
	return false;
}

} // namespace

bool CompilePredicateMappings(const RelationDeclaration &relation, const std::string &package_digest,
                              const std::vector<CompiledOperation> &operations, PackageDiagnosticSink &diagnostics,
                              std::vector<CompiledPredicateMapping> &mappings) {
	for (const auto &predicate : relation.predicates) {
		const auto *column = FindColumn(relation, predicate.column.value);
		if (column == nullptr) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_REFERENCE, PackageDiagnosticPhase::REFERENCE,
			                predicate.column.mark, "", relation.id.value);
			continue;
		}
		if (column->type.value != predicate.literal_type.value) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_PREDICATE, PackageDiagnosticPhase::COMPILE,
			                predicate.literal_type.mark, "", relation.id.value);
			continue;
		}
		auto literal = CompileConcreteScalar(predicate.literal_type, predicate.literal_value, diagnostics,
		                                     relation.id.value, PackageDiagnosticCode::INVALID_TYPE);
		std::string encoded_literal;
		try {
			encoded_literal = EncodeCompiledQueryScalar(literal);
		} catch (const std::invalid_argument &) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                predicate.literal_value.mark, "", relation.id.value);
			continue;
		}
		if (predicate.matching_fixture.value == predicate.false_or_null_fixture.value ||
		    predicate.matching_fixture.value == predicate.duplicates_fixture.value ||
		    predicate.false_or_null_fixture.value == predicate.duplicates_fixture.value) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_PREDICATE, PackageDiagnosticPhase::COMPILE, predicate.mark,
			                "", relation.id.value);
		}
		std::set<std::string> targets;
		for (const auto &target : predicate.operations) {
			const auto *operation = FindOperation(operations, target.value);
			if (!targets.insert(target.value).second) {
				diagnostics.Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, target.mark, "",
				                relation.id.value);
				continue;
			}
			if (operation == nullptr || operation->Protocol() != CompiledProtocol::REST) {
				diagnostics.Add(PackageDiagnosticCode::INVALID_REFERENCE, PackageDiagnosticPhase::REFERENCE,
				                target.mark, "", relation.id.value);
				continue;
			}
			if (!HasConditionalBinding(*operation, predicate.conditional_input.value)) {
				diagnostics.Add(PackageDiagnosticCode::INVALID_PREDICATE, PackageDiagnosticPhase::COMPILE,
				                predicate.conditional_input.mark, "", relation.id.value, operation->name);
				continue;
			}
			bool ambiguous = false;
			for (const auto &mapping : mappings) {
				if (mapping.OperationName() != operation->name) {
					continue;
				}
				ambiguous = mapping.RemoteInputName() != predicate.conditional_input.value ||
				            SameScalar(mapping.TypedLiteral(), literal) ||
				            mapping.EncodedRemoteValue() == encoded_literal;
				if (ambiguous) {
					break;
				}
			}
			if (ambiguous) {
				diagnostics.Add(PackageDiagnosticCode::INVALID_PREDICATE, PackageDiagnosticPhase::COMPILE, target.mark,
				                "", relation.id.value, operation->name);
				continue;
			}
			const auto identities =
			    cuac::internal::DerivePackagePredicateIdentities(package_digest, relation.id.value, *operation);
			mappings.push_back(cuac::internal::CompiledModelBuilder::PackagePredicate(
			    predicate.id.value, predicate.column.value, literal, operation->name, predicate.conditional_input.value,
			    encoded_literal,
			    predicate.accuracy.value == "exact" ? CompiledPredicateAccuracy::EXACT
			                                        : CompiledPredicateAccuracy::SUPERSET,
			    identities.proof, identities.base_domain, predicate.matching_fixture.value,
			    predicate.false_or_null_fixture.value, predicate.duplicates_fixture.value));
		}
	}
	return diagnostics.Empty();
}

} // namespace internal
} // namespace connector
} // namespace cuac
