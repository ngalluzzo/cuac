#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

#include "cuac/connector/package_semver.hpp"

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

OriginDeclaration DecodeOrigin(const SchemaReader &reader) {
	OriginDeclaration origin;
	reader.RequireMapping({"scheme", "host", "port"}, {"scheme", "host", "port"});
	origin.scheme = reader.Text("scheme");
	origin.host = reader.Text("host");
	origin.port = reader.Text("port");
	origin.mark = reader.Mark();
	RequireValue(origin.scheme, "https", PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
	             reader.Diagnostics());
	if (!IsCanonicalHost(origin.host.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, origin.host.mark);
	}
	std::uint64_t port = 0;
	if (!IsCanonicalUnsigned(origin.port, port) || port > 65535) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, origin.port.mark);
	}
	return origin;
}

std::vector<OriginDeclaration> DecodeOrigins(const SchemaReader &reader, const std::string &name, std::size_t minimum) {
	std::vector<OriginDeclaration> result;
	const auto *sequence = reader.Sequence(name, minimum, 64);
	if (sequence == nullptr) {
		return result;
	}
	result.reserve(sequence->Size());
	std::set<std::string> identities;
	for (std::size_t index = 0; index < sequence->Size(); index++) {
		auto child = reader.Child(sequence->SequenceValue(index), "." + name + "[" + std::to_string(index) + "]");
		auto origin = DecodeOrigin(child);
		const auto identity = origin.scheme.value + "\n" + origin.host.value + "\n" + origin.port.value;
		if (!identities.insert(identity).second) {
			reader.Diagnostics().Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, origin.mark);
		}
		result.push_back(std::move(origin));
	}
	return result;
}

CredentialDeclaration DecodeCredential(const SchemaReader &reader) {
	CredentialDeclaration credential;
	reader.RequireMapping({"id", "kind", "secret_field", "placement", "header_name", "query_param", "destinations"},
	                      {"id", "kind", "secret_field", "placement", "destinations"});
	credential.id = reader.Text("id");
	credential.kind = reader.Text("kind");
	credential.secret_field = reader.Text("secret_field");
	credential.placement = reader.Text("placement");
	credential.header_name = reader.Text("header_name", false);
	credential.query_param = reader.Text("query_param", false);
	credential.destinations = DecodeOrigins(reader, "destinations", 1);
	credential.mark = reader.Mark();
	RequireIdentifier(credential.id, reader.Diagnostics());
	RequireValue(credential.secret_field, "token", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
	if (credential.kind.value == "bearer") {
		RequireValue(credential.placement, "authorization_header", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
		             PackageDiagnosticPhase::SCHEMA, reader.Diagnostics());
		for (const auto *name : {"header_name", "query_param"}) {
			if (reader.Field(name) != nullptr) {
				reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
				                         reader.FieldMark(name));
			}
		}
	} else if (credential.kind.value == "api_key") {
		if (credential.placement.value == "header") {
			if (reader.Field("header_name") == nullptr) {
				reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
				                         reader.FieldMark("header_name"));
			} else if (!IsHeaderName(credential.header_name.value)) {
				reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
				                         credential.header_name.mark);
			}
			if (reader.Field("query_param") != nullptr) {
				reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
				                         reader.FieldMark("query_param"));
			}
		} else if (credential.placement.value == "query") {
			if (reader.Field("query_param") == nullptr) {
				reader.Diagnostics().Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA,
				                         reader.FieldMark("query_param"));
			} else if (!IsQueryName(credential.query_param.value)) {
				reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
				                         credential.query_param.mark);
			}
			if (reader.Field("header_name") != nullptr) {
				reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
				                         reader.FieldMark("header_name"));
			}
		} else {
			reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                         credential.placement.mark);
			for (const auto *name : {"header_name", "query_param"}) {
				if (reader.Field(name) != nullptr) {
					reader.Diagnostics().Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA,
					                         reader.FieldMark(name));
				}
			}
		}
	} else {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         credential.kind.mark);
	}
	return credential;
}

NetworkPolicyDeclaration DecodeNetworkPolicy(const SchemaReader &reader) {
	NetworkPolicyDeclaration policy;
	reader.RequireMapping({"origins", "redirects", "private_addresses", "link_local_addresses", "loopback_addresses",
	                       "max_response_bytes"},
	                      {"origins", "redirects", "private_addresses", "link_local_addresses", "loopback_addresses",
	                       "max_response_bytes"});
	policy.origins = DecodeOrigins(reader, "origins", 1);
	policy.redirects = reader.Text("redirects");
	policy.private_addresses = reader.Text("private_addresses");
	policy.link_local_addresses = reader.Text("link_local_addresses");
	policy.loopback_addresses = reader.Text("loopback_addresses");
	policy.max_response_bytes = reader.Text("max_response_bytes");
	policy.mark = reader.Mark();
	RequireValue(policy.redirects, "deny", PackageDiagnosticCode::POLICY_WIDENING, PackageDiagnosticPhase::COMPILE,
	             reader.Diagnostics());
	RequireValue(policy.private_addresses, "deny", PackageDiagnosticCode::POLICY_WIDENING,
	             PackageDiagnosticPhase::COMPILE, reader.Diagnostics());
	RequireValue(policy.link_local_addresses, "deny", PackageDiagnosticCode::POLICY_WIDENING,
	             PackageDiagnosticPhase::COMPILE, reader.Diagnostics());
	RequireValue(policy.loopback_addresses, "deny", PackageDiagnosticCode::POLICY_WIDENING,
	             PackageDiagnosticPhase::COMPILE, reader.Diagnostics());
	std::uint64_t response_bytes = 0;
	if (!IsCanonicalUnsigned(policy.max_response_bytes, response_bytes)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA,
		                         policy.max_response_bytes.mark);
	}
	return policy;
}

} // namespace

bool DecodeManifestSchema(const std::string &file, const FailsafeYamlNode &root, PackageDiagnosticSink &diagnostics,
                          ManifestDeclaration &manifest) {
	SchemaReader reader(file, root, "$", diagnostics);
	if (!reader.RequireMapping(
	        {"api_version", "kind", "id", "version", "extractor_dialect", "credentials", "network_policy", "relations"},
	        {"api_version", "kind", "id", "version", "extractor_dialect", "network_policy", "relations"})) {
		return false;
	}
	manifest.api_version = reader.Text("api_version");
	manifest.kind = reader.Text("kind");
	manifest.id = reader.Text("id");
	manifest.version = reader.Text("version");
	manifest.extractor_dialect = reader.Text("extractor_dialect");
	manifest.network_policy = DecodeNetworkPolicy(reader.Child("network_policy"));
	manifest.relations = reader.TextSequence("relations", 1, 64);
	manifest.mark = reader.Mark();

	if (manifest.api_version.value != "cuac/v1") {
		diagnostics.Add(PackageDiagnosticCode::UNSUPPORTED_SPEC, PackageDiagnosticPhase::SCHEMA,
		                manifest.api_version.mark);
	}
	RequireValue(manifest.kind, "connector", PackageDiagnosticCode::UNSUPPORTED_DECLARATION,
	             PackageDiagnosticPhase::SCHEMA, diagnostics);
	RequireIdentifier(manifest.id, diagnostics);
	try {
		(void)PackageSemVer::Parse(manifest.version.value);
	} catch (const std::exception &) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, manifest.version.mark);
	}
	RequireValue(manifest.extractor_dialect, "cuac/json_path_v1", PackageDiagnosticCode::UNSUPPORTED_DIALECT,
	             PackageDiagnosticPhase::SCHEMA, diagnostics);

	std::set<std::string> relation_ids;
	for (const auto &relation : manifest.relations) {
		RequireIdentifier(relation, diagnostics);
		if (!relation_ids.insert(relation.value).second) {
			diagnostics.Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, relation.mark,
			                manifest.id.value);
		}
	}

	const auto *credentials = reader.Sequence("credentials", 0, 64);
	if (credentials != nullptr) {
		std::set<std::string> credential_ids;
		manifest.credentials.reserve(credentials->Size());
		for (std::size_t index = 0; index < credentials->Size(); index++) {
			auto credential = DecodeCredential(
			    reader.Child(credentials->SequenceValue(index), ".credentials[" + std::to_string(index) + "]"));
			if (!credential_ids.insert(credential.id.value).second) {
				diagnostics.Add(PackageDiagnosticCode::DUPLICATE_ID, PackageDiagnosticPhase::SCHEMA, credential.id.mark,
				                manifest.id.value);
			}
			manifest.credentials.push_back(std::move(credential));
		}
	}
	return diagnostics.Empty();
}

} // namespace internal
} // namespace connector
} // namespace cuac
