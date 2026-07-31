#include "cuac/internal/connector/compiler/package_relation_schema_parts.hpp"

#include <set>

namespace cuac {
namespace connector {
namespace internal {

namespace {

void RequireValue(const LocatedText &value, const char *expected, PackageDiagnosticCode code,
                  PackageDiagnosticPhase phase, PackageDiagnosticSink &diagnostics) {
	if (value.value != expected) {
		diagnostics.Add(code, phase, value.mark);
	}
}

void RequireIdentifier(const LocatedText &value, PackageDiagnosticSink &diagnostics) {
	if (!IsIdentifier(value.value)) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_IDENTIFIER, PackageDiagnosticPhase::SCHEMA, value.mark);
	}
}

SelectorDeclaration DecodeSelector(const SchemaReader &reader) {
	SelectorDeclaration selector;
	selector.fallback = reader.Field("fallback") != nullptr;
	selector.mark = reader.Mark();
	if (selector.fallback) {
		RequireValue(reader.Text("fallback"), "true", PackageDiagnosticCode::INVALID_SELECTOR,
		             PackageDiagnosticPhase::COMPILE, reader.Diagnostics());
		if (reader.Field("when") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_SELECTOR, PackageDiagnosticPhase::COMPILE,
			                         reader.FieldMark("when"));
		}
		return selector;
	}
	if (reader.Field("when") == nullptr) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_SELECTOR, PackageDiagnosticPhase::COMPILE,
		                         reader.Mark());
		return selector;
	}
	auto when = reader.Child("when");
	when.RequireMapping({"required_inputs"}, {"required_inputs"});
	selector.required_inputs = when.TextSequence("required_inputs", 1, 128);
	return selector;
}

RetryDeclaration DecodeRetry(const SchemaReader &reader) {
	RetryDeclaration retry;
	retry.present = true;
	retry.mark = reader.Mark();
	reader.RequireMapping(
	    {"max_attempts_per_step", "max_delay_milliseconds", "max_cumulative_waiting_milliseconds_per_scan"},
	    {"max_attempts_per_step", "max_delay_milliseconds", "max_cumulative_waiting_milliseconds_per_scan"});
	retry.max_attempts_per_step = reader.Text("max_attempts_per_step");
	retry.max_delay_milliseconds = reader.Text("max_delay_milliseconds");
	retry.max_cumulative_waiting_milliseconds_per_scan = reader.Text("max_cumulative_waiting_milliseconds_per_scan");
	std::uint64_t attempts = 0;
	std::uint64_t delay = 0;
	std::uint64_t waiting = 0;
	if (!IsCanonicalUnsigned(retry.max_attempts_per_step, attempts) || attempts < 2 || attempts > 3) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         retry.max_attempts_per_step.mark);
	}
	if (!IsCanonicalUnsigned(retry.max_delay_milliseconds, delay) || delay == 0 || delay > 100) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         retry.max_delay_milliseconds.mark);
	}
	if (!IsCanonicalUnsigned(retry.max_cumulative_waiting_milliseconds_per_scan, waiting) || waiting == 0 ||
	    waiting > 250) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         retry.max_cumulative_waiting_milliseconds_per_scan.mark);
	}
	return retry;
}

void RequireRateLimitField(const SchemaReader &reader, const char *name) {
	if (reader.Field(name) == nullptr) {
		reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
		                         reader.FieldMark(name));
	}
}

void ForbidRateLimitField(const SchemaReader &reader, const char *name) {
	if (reader.Field(name) != nullptr) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         reader.FieldMark(name));
	}
}

RateLimitGuidanceDeclaration DecodeRateLimitGuidance(const SchemaReader &reader) {
	RateLimitGuidanceDeclaration guidance;
	guidance.mark = reader.Mark();
	reader.RequireMapping({"header", "format"}, {"header", "format"});
	guidance.header = reader.Text("header");
	guidance.format = reader.Text("format");
	if (!IsRateLimitHeaderName(guidance.header.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         guidance.header.mark);
	}
	if (guidance.format.value != "retry_after" && guidance.format.value != "delta_seconds" &&
	    guidance.format.value != "unix_seconds") {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         guidance.format.mark);
	}
	return guidance;
}

LocatedText DecodeRateLimitHeaderRole(const SchemaReader &reader) {
	reader.RequireMapping({"header"}, {"header"});
	auto header = reader.Text("header");
	if (!IsRateLimitHeaderName(header.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         header.mark);
	}
	return header;
}

RateLimitDeclaration DecodeRateLimit(const SchemaReader &reader) {
	RateLimitDeclaration rate_limit;
	rate_limit.present = true;
	rate_limit.remaining_present = reader.Field("remaining") != nullptr;
	rate_limit.remote_bucket_present = reader.Field("remote_bucket") != nullptr;
	rate_limit.mark = reader.Mark();
	reader.RequireMapping({"statuses", "mode", "operation_family", "principal_scope", "guidance", "remaining",
	                       "remote_bucket", "max_attempts_per_step", "max_delay_milliseconds",
	                       "max_cumulative_waiting_milliseconds_per_scan"},
	                      {"statuses", "mode", "operation_family", "principal_scope"});
	rate_limit.statuses = reader.TextSequence("statuses", 1, 8);
	rate_limit.mode = reader.Text("mode");
	rate_limit.operation_family = reader.Text("operation_family");
	rate_limit.principal_scope = reader.Text("principal_scope");

	std::set<std::uint64_t> statuses;
	for (const auto &source : rate_limit.statuses) {
		std::uint64_t status = 0;
		if (source.value.size() != 3 || !IsCanonicalUnsigned(source, status) || status < 400 || status > 599 ||
		    status == 401 || status == 403 || status == 407) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         source.mark);
		} else if (!statuses.insert(status).second) {
			reader.Diagnostics().Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, source.mark);
		}
	}
	if (rate_limit.mode.value != "fail" && rate_limit.mode.value != "wait" &&
	    rate_limit.mode.value != "wait_if_deadline_allows") {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         rate_limit.mode.mark);
	}
	if (!IsRateLimitOperationFamily(rate_limit.operation_family.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_IDENTIFIER, PackageDiagnosticPhase::SCHEMA,
		                         rate_limit.operation_family.mark);
	}
	if (rate_limit.principal_scope.value != "credential_authority" && rate_limit.principal_scope.value != "shared") {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         rate_limit.principal_scope.mark);
	}

	if (reader.Field("guidance") != nullptr) {
		const auto *sequence = reader.Sequence("guidance", 1, 4);
		if (sequence != nullptr) {
			for (std::size_t index = 0; index < sequence->Size(); index++) {
				rate_limit.guidance.push_back(DecodeRateLimitGuidance(
				    reader.Child(sequence->SequenceValue(index), ".guidance[" + std::to_string(index) + "]")));
			}
		}
	}
	if (rate_limit.remaining_present) {
		rate_limit.remaining_header = DecodeRateLimitHeaderRole(reader.Child("remaining"));
	}
	if (rate_limit.remote_bucket_present) {
		rate_limit.remote_bucket_header = DecodeRateLimitHeaderRole(reader.Child("remote_bucket"));
	}

	std::set<std::string> headers;
	for (const auto &guidance : rate_limit.guidance) {
		if (!headers.insert(guidance.header.value).second) {
			reader.Diagnostics().Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA,
			                         guidance.header.mark);
		}
	}
	for (const auto *header : {rate_limit.remaining_present ? &rate_limit.remaining_header : nullptr,
	                           rate_limit.remote_bucket_present ? &rate_limit.remote_bucket_header : nullptr}) {
		if (header != nullptr && !headers.insert(header->value).second) {
			reader.Diagnostics().Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, header->mark);
		}
	}

	if (rate_limit.mode.value == "fail") {
		for (const auto *name : {"guidance", "remaining", "remote_bucket", "max_attempts_per_step",
		                         "max_delay_milliseconds", "max_cumulative_waiting_milliseconds_per_scan"}) {
			ForbidRateLimitField(reader, name);
		}
		return rate_limit;
	}
	if (rate_limit.mode.value == "wait" || rate_limit.mode.value == "wait_if_deadline_allows") {
		for (const auto *name : {"guidance", "max_attempts_per_step", "max_delay_milliseconds",
		                         "max_cumulative_waiting_milliseconds_per_scan"}) {
			RequireRateLimitField(reader, name);
		}
		rate_limit.max_attempts_per_step = reader.Text("max_attempts_per_step");
		rate_limit.max_delay_milliseconds = reader.Text("max_delay_milliseconds");
		rate_limit.max_cumulative_waiting_milliseconds_per_scan =
		    reader.Text("max_cumulative_waiting_milliseconds_per_scan");
		std::uint64_t attempts = 0;
		std::uint64_t delay = 0;
		std::uint64_t waiting = 0;
		if (!IsCanonicalUnsigned(rate_limit.max_attempts_per_step, attempts) || attempts < 2 || attempts > 3) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         rate_limit.max_attempts_per_step.mark);
		}
		if (!IsCanonicalUnsigned(rate_limit.max_delay_milliseconds, delay) || delay > 30000) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         rate_limit.max_delay_milliseconds.mark);
		}
		if (!IsCanonicalUnsigned(rate_limit.max_cumulative_waiting_milliseconds_per_scan, waiting) || waiting > 30000) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         rate_limit.max_cumulative_waiting_milliseconds_per_scan.mark);
		}
	}
	return rate_limit;
}

} // namespace

OperationDeclaration DecodeOperationSchema(const SchemaReader &reader) {
	OperationDeclaration operation;
	reader.RequireMapping({"id", "fallback", "when", "cardinality", "replay_safety", "retry", "rate_limit", "request",
	                       "response", "pagination"},
	                      {"id", "cardinality", "replay_safety", "request"});
	operation.id = reader.Text("id");
	operation.cardinality = reader.Text("cardinality");
	operation.replay_safety = reader.Text("replay_safety");
	operation.retry.present = false;
	operation.rate_limit.present = false;
	if (reader.Field("retry") != nullptr) {
		operation.retry = DecodeRetry(reader.Child("retry"));
	}
	if (reader.Field("rate_limit") != nullptr) {
		operation.rate_limit = DecodeRateLimit(reader.Child("rate_limit"));
	}
	operation.selector = DecodeSelector(reader);
	operation.mark = reader.Mark();
	RequireIdentifier(operation.id, reader.Diagnostics());
	RequireValue(operation.replay_safety, "safe", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	const auto *request_node = reader.Field("request");
	if (request_node == nullptr || request_node->Type() != FailsafeYamlNode::Kind::MAPPING) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                         reader.FieldMark("request"));
		return operation;
	}
	auto request = reader.Child("request");
	const auto protocol = request.Text("protocol");
	operation.graphql = protocol.value == "graphql";
	if (operation.graphql) {
		if (reader.Field("response") != nullptr || reader.Field("pagination") != nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         operation.mark);
		}
		RequireValue(operation.cardinality, "many", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
		             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
		operation.graphql_request = DecodeGraphqlRequestSchema(request);
	} else {
		if (reader.Field("response") == nullptr || reader.Field("pagination") == nullptr) {
			reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
			                         operation.mark);
		}
		if (operation.cardinality.value != "one" && operation.cardinality.value != "many") {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         operation.cardinality.mark);
		}
		operation.rest = DecodeRestRequestSchema(request);
		operation.response = DecodeRestResponseSchema(reader.Child("response"));
		operation.rest_pagination = DecodeRestPaginationSchema(reader.Child("pagination"));
	}
	return operation;
}

} // namespace internal
} // namespace connector
} // namespace cuac
