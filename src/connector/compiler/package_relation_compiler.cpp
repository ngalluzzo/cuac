#include "cuac/internal/connector/compiler/package_model_compiler.hpp"

#include <limits>

namespace cuac {
namespace connector {
namespace internal {

namespace {

const CredentialDeclaration *FindCredential(const ManifestDeclaration &manifest, const std::string &id) {
	for (const auto &credential : manifest.credentials) {
		if (credential.id.value == id) {
			return &credential;
		}
	}
	return nullptr;
}

bool SameOrigin(const OriginDeclaration &left, const OriginDeclaration &right) {
	return left.scheme.value == right.scheme.value && left.host.value == right.host.value &&
	       left.port.value == right.port.value;
}

CompiledInputDefault CompileDefault(const InputDeclaration &input, const std::string &relation,
                                    PackageDiagnosticSink &diagnostics) {
	if (!input.default_value.present) {
		return cuac::internal::CompiledModelBuilder::NoDefault();
	}
	if (input.default_value.kind.value == "null") {
		if (!ParseBoolean(input.nullable)) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
			                input.default_value.kind.mark, "", relation);
			return cuac::internal::CompiledModelBuilder::NoDefault();
		}
		return cuac::internal::CompiledModelBuilder::Default(
		    cuac::internal::CompiledModelBuilder::Null(ScalarType(input.type)));
	}
	if (input.type.value == "VARCHAR" &&
	    input.default_value.value.style != FailsafeYamlNode::ScalarStyle::DOUBLE_QUOTED) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                input.default_value.value.mark, "", relation);
	}
	return cuac::internal::CompiledModelBuilder::Default(CompileConcreteScalar(
	    input.type, input.default_value.value, diagnostics, relation, PackageDiagnosticCode::INVALID_TYPE));
}

std::vector<CompiledColumn> CompileColumns(const RelationDeclaration &relation, PackageDiagnosticSink &diagnostics) {
	std::vector<CompiledColumn> result;
	result.reserve(relation.columns.size());
	for (const auto &column : relation.columns) {
		std::vector<std::string> segments;
		if (!IsExtractor(column.extract.value, false, &segments)) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_EXTRACTOR, PackageDiagnosticPhase::SCHEMA,
			                column.extract.mark, "", relation.id.value);
			continue;
		}
		if (column.type.value == "ARRAY") {
			result.push_back(cuac::internal::CompiledModelBuilder::ArrayColumn(
			    column.id.value, ScalarType(column.element_type), ParseBoolean(column.element_nullable),
			    ParseBoolean(column.nullable), column.extract.value, std::move(segments)));
		} else {
			result.push_back(cuac::internal::CompiledModelBuilder::Column(column.id.value, ScalarType(column.type),
			                                                              ParseBoolean(column.nullable),
			                                                              column.extract.value, std::move(segments)));
		}
	}
	return result;
}

std::vector<CompiledRelationInput> CompileInputs(const RelationDeclaration &relation,
                                                 PackageDiagnosticSink &diagnostics) {
	std::vector<CompiledRelationInput> result;
	result.reserve(relation.inputs.size());
	for (const auto &input : relation.inputs) {
		result.push_back(cuac::internal::CompiledModelBuilder::Input(
		    input.id.value, ScalarType(input.type), ParseBoolean(input.nullable),
		    CompileDefault(input, relation.id.value, diagnostics)));
	}
	return result;
}

CompiledAuthenticationPolicy CompileAuthentication(const ManifestDeclaration &manifest,
                                                   const RelationDeclaration &relation,
                                                   PackageDiagnosticSink &diagnostics) {
	if (relation.auth.mode.value == "anonymous") {
		return cuac::internal::CompiledModelBuilder::AnonymousAuthentication();
	}
	const auto *credential = FindCredential(manifest, relation.auth.credential.value);
	if (credential == nullptr) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_REFERENCE, PackageDiagnosticPhase::REFERENCE,
		                relation.auth.credential.mark, manifest.id.value, relation.id.value);
		return cuac::internal::CompiledModelBuilder::AnonymousAuthentication();
	}
	std::vector<CompiledHttpOrigin> destinations;
	for (const auto &destination : credential->destinations) {
		destinations.push_back(CompileOrigin(destination));
	}
	for (const auto &operation : relation.operations) {
		const auto &origin = operation.graphql ? operation.graphql_request.origin : operation.rest.origin;
		bool allowed = false;
		for (const auto &destination : credential->destinations) {
			allowed = allowed || SameOrigin(origin, destination);
		}
		if (!allowed) {
			diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE, origin.mark,
			                manifest.id.value, relation.id.value, operation.id.value);
		}
		// api_key credentials place their value in a header or query
		// parameter Remote Runtime's GraphQL admission does not construct
		// (GraphQL requests are a fixed JSON POST body with no author-facing
		// query-parameter surface, and header placement is not yet
		// implemented for GraphQL). Rejecting this combination here, rather
		// than only failing at execution with a generic policy diagnostic,
		// gives the author a precise compile-time signal.
		if (credential->kind.value == "api_key" && operation.graphql) {
			diagnostics.Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::COMPILE,
			                operation.mark, manifest.id.value, relation.id.value, operation.id.value);
		}
	}
	if (credential->kind.value == "api_key") {
		const bool header_placement = credential->placement.value == "header";
		const auto placement =
		    header_placement ? CompiledCredentialPlacement::HEADER_NAMED : CompiledCredentialPlacement::QUERY_NAMED;
		const auto &placement_name = header_placement ? credential->header_name.value : credential->query_param.value;
		return cuac::internal::CompiledModelBuilder::ApiKeyAuthentication(credential->secret_field.value, placement,
		                                                                  placement_name, std::move(destinations));
	}
	return cuac::internal::CompiledModelBuilder::BearerAuthentication(credential->secret_field.value,
	                                                                  std::move(destinations));
}

CompiledResourceCeilings CompileResources(const RelationDeclaration &relation, PackageDiagnosticSink &diagnostics) {
	const auto page_bytes = ParseUnsigned(relation.resources.max_response_bytes_per_page);
	const auto scan_bytes = ParseUnsigned(relation.resources.max_response_bytes_per_scan);
	const auto page_records = ParseUnsigned(relation.resources.max_records_per_page);
	const auto scan_records = ParseUnsigned(relation.resources.max_records_per_scan);
	const auto string_bytes = ParseUnsigned(relation.resources.max_extracted_string_bytes);
	if (scan_bytes < page_bytes || scan_records < page_records) {
		diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
		                relation.resources.mark, "", relation.id.value);
	}
	for (const auto &operation : relation.operations) {
		if (operation.cardinality.value == "one" && (page_records != 1 || scan_records != 1)) {
			diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
			                relation.resources.mark, "", relation.id.value, operation.id.value);
		}
		if (!operation.graphql && operation.rest_pagination.strategy.value == "disabled") {
			if (scan_bytes != page_bytes || scan_records != page_records) {
				diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
				                relation.resources.mark, "", relation.id.value, operation.id.value);
			}
			continue;
		}
		const auto operation_page_size = operation.graphql
		                                     ? ParseUnsigned(operation.graphql_request.pagination.page_size)
		                                     : ParseUnsigned(operation.rest_pagination.page_size);
		const auto operation_pages = operation.graphql
		                                 ? ParseUnsigned(operation.graphql_request.pagination.max_pages_per_scan)
		                                 : ParseUnsigned(operation.rest_pagination.max_pages_per_scan);
		if (operation_page_size > page_records || operation_pages == 0 ||
		    page_records > std::numeric_limits<std::uint64_t>::max() / operation_pages ||
		    page_bytes > std::numeric_limits<std::uint64_t>::max() / operation_pages ||
		    scan_records > page_records * operation_pages || scan_bytes > page_bytes * operation_pages) {
			diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
			                relation.resources.mark, "", relation.id.value, operation.id.value);
		}
		if (!operation.graphql) {
			continue;
		}
		const auto request_bytes = ParseUnsigned(operation.graphql_request.max_serialized_body_bytes_per_request);
		const auto scan_request_bytes = ParseUnsigned(operation.graphql_request.max_serialized_body_bytes_per_scan);
		const auto pages = ParseUnsigned(operation.graphql_request.pagination.max_pages_per_scan);
		const auto retry_attempts = operation.retry.present ? ParseUnsigned(operation.retry.max_attempts_per_step) : 1;
		const auto rate_limit_attempts = operation.rate_limit.present && operation.rate_limit.mode.value != "fail"
		                                     ? ParseUnsigned(operation.rate_limit.max_attempts_per_step)
		                                     : 1;
		const auto attempts = retry_attempts > rate_limit_attempts ? retry_attempts : rate_limit_attempts;
		const bool aggregate_overflow =
		    pages == 0 || attempts == 0 || pages > std::numeric_limits<std::uint64_t>::max() / attempts;
		const auto aggregate_attempts = aggregate_overflow ? 0 : pages * attempts;
		if (aggregate_overflow || request_bytes > std::numeric_limits<std::uint64_t>::max() / aggregate_attempts ||
		    scan_request_bytes < request_bytes || scan_request_bytes > request_bytes * aggregate_attempts) {
			diagnostics.Add(PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
			                operation.graphql_request.mark, "", relation.id.value, operation.id.value);
		}
	}
	return cuac::internal::CompiledModelBuilder::Resources(page_bytes, scan_bytes, page_records, scan_records,
	                                                       string_bytes);
}

} // namespace

std::unique_ptr<CompiledRelation> CompileRelation(const ManifestDeclaration &manifest,
                                                  const RelationDeclaration &relation,
                                                  const std::string &package_digest, PackageDiagnosticSink &diagnostics,
                                                  PackageCancellation &cancellation) {
	const auto revision = diagnostics.Revision();
	if (cancellation.IsCancellationRequested()) {
		throw FailsafeYamlError(FailsafeYamlErrorCode::CANCELLED, relation.mark.file, relation.mark.span,
		                        "package compilation was cancelled");
	}
	auto columns = CompileColumns(relation, diagnostics);
	auto inputs = CompileInputs(relation, diagnostics);
	std::vector<CompiledOperation> operations;
	CompileOperations(relation, diagnostics, operations);
	std::vector<CompiledPredicateMapping> predicates;
	CompilePredicateMappings(relation, package_digest, operations, diagnostics, predicates);
	auto authentication = CompileAuthentication(manifest, relation, diagnostics);
	auto resources = CompileResources(relation, diagnostics);
	if (diagnostics.Revision() != revision) {
		return nullptr;
	}
	return std::unique_ptr<CompiledRelation>(new CompiledRelation(cuac::internal::CompiledModelBuilder::Relation(
	    relation.id.value, std::move(columns), std::move(inputs), std::move(predicates), std::move(operations),
	    std::move(authentication), std::move(resources))));
}

} // namespace internal
} // namespace connector
} // namespace cuac
