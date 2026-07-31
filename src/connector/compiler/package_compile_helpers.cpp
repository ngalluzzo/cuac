#include "cuac/internal/connector/compiler/package_model_compiler.hpp"

#include <cctype>
#include <set>

namespace cuac {
namespace connector {
namespace internal {

namespace {

char AsciiLower(char value) {
	return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
}

std::string Lower(const std::string &value) {
	std::string result;
	result.reserve(value.size());
	for (const auto character : value) {
		result.push_back(AsciiLower(character));
	}
	return result;
}

bool ContainsCredentialHint(const std::string &lower) {
	return lower.find("token") != std::string::npos || lower.find("secret") != std::string::npos ||
	       lower.find("api-key") != std::string::npos || lower.find("apikey") != std::string::npos;
}

bool ReservedHeader(const std::string &name, bool graphql) {
	const auto lower = Lower(name);
	static const std::set<std::string> reserved = {"authorization",
	                                               "proxy-authorization",
	                                               "host",
	                                               "connection",
	                                               "content-length",
	                                               "transfer-encoding",
	                                               "trailer",
	                                               "te",
	                                               "upgrade",
	                                               "keep-alive",
	                                               "proxy-connection",
	                                               "expect",
	                                               "range",
	                                               "cookie",
	                                               "set-cookie",
	                                               "accept-encoding"};
	return reserved.count(lower) != 0 || ContainsCredentialHint(lower) || (graphql && lower == "content-type");
}

bool SafeVarchar(const LocatedText &value, bool require_double_quoted) {
	if (require_double_quoted && value.style != FailsafeYamlNode::ScalarStyle::DOUBLE_QUOTED) {
		return false;
	}
	for (const auto character : value.value) {
		if (static_cast<unsigned char>(character) < 0x20U) {
			return false;
		}
	}
	return true;
}

} // namespace

CompiledScalarType ScalarType(const LocatedText &type) {
	if (type.value == "BOOLEAN") {
		return CompiledScalarType::BOOLEAN;
	}
	if (type.value == "BIGINT") {
		return CompiledScalarType::BIGINT;
	}
	if (type.value == "DOUBLE") {
		return CompiledScalarType::DOUBLE;
	}
	return CompiledScalarType::VARCHAR;
}

CompiledGraphqlScalarKind GraphqlScalarType(const LocatedText &type) {
	if (type.value == "BOOLEAN") {
		return CompiledGraphqlScalarKind::BOOLEAN;
	}
	if (type.value == "BIGINT") {
		return CompiledGraphqlScalarKind::INT64;
	}
	return CompiledGraphqlScalarKind::STRING;
}

CompiledHttpOrigin CompileOrigin(const OriginDeclaration &origin) {
	std::uint64_t port = 0;
	(void)IsCanonicalUnsigned(origin.port, port);
	return {CompiledUrlScheme::HTTPS, CompiledHttpHost(origin.host.value), static_cast<std::uint16_t>(port)};
}

bool ParseBoolean(const LocatedText &value) {
	bool result = false;
	(void)IsPlainBoolean(value, result);
	return result;
}

std::uint64_t ParseUnsigned(const LocatedText &value) {
	std::uint64_t result = 0;
	(void)IsCanonicalUnsigned(value, result);
	return result;
}

CompiledScalarValue CompileConcreteScalar(const LocatedText &type, const LocatedText &value,
                                          PackageDiagnosticSink &diagnostics, const std::string &relation,
                                          PackageDiagnosticCode code) {
	if (type.value == "BOOLEAN") {
		bool parsed = false;
		if (!IsPlainBoolean(value, parsed)) {
			diagnostics.Add(code, PackageDiagnosticPhase::SCHEMA, value.mark, "", relation);
		}
		return cuac::internal::CompiledModelBuilder::Boolean(parsed);
	}
	if (type.value == "BIGINT") {
		std::int64_t parsed = 0;
		if (!IsCanonicalSigned(value, parsed)) {
			diagnostics.Add(code, PackageDiagnosticPhase::SCHEMA, value.mark, "", relation);
		}
		return cuac::internal::CompiledModelBuilder::Bigint(parsed);
	}
	if (type.value == "DOUBLE") {
		double parsed = 0.0;
		if (!IsCanonicalDouble(value, parsed)) {
			diagnostics.Add(code, PackageDiagnosticPhase::SCHEMA, value.mark, "", relation);
		}
		return cuac::internal::CompiledModelBuilder::Double(parsed);
	}
	if (!SafeVarchar(value, false)) {
		diagnostics.Add(code, PackageDiagnosticPhase::SCHEMA, value.mark, "", relation);
	}
	return cuac::internal::CompiledModelBuilder::Varchar(value.value);
}

std::vector<CompiledHttpHeader> CompileHeaders(const std::vector<HeaderDeclaration> &headers, bool graphql,
                                               const RelationDeclaration &relation,
                                               const OperationDeclaration &operation,
                                               PackageDiagnosticSink &diagnostics) {
	std::vector<CompiledHttpHeader> result;
	std::set<std::string> names;
	std::uint64_t total_bytes = 0;
	bool content_type_inserted = false;
	for (const auto &header : headers) {
		const auto normalized = Lower(header.name.value);
		if (!names.insert(normalized).second || ReservedHeader(header.name.value, graphql)) {
			diagnostics.Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
			                header.name.mark, "", relation.id.value, operation.id.value);
		}
		total_bytes += header.name.value.size() + header.value.value.size();
		result.push_back({header.name.value, header.value.value});
		if (graphql && normalized == "accept") {
			result.push_back({"Content-Type", "application/json"});
			content_type_inserted = true;
		}
	}
	if (graphql && !content_type_inserted) {
		result.insert(result.begin(), {"Content-Type", "application/json"});
	}
	if (total_bytes > 16 * 1024) {
		diagnostics.Add(PackageDiagnosticCode::RESOURCE_EXHAUSTED, PackageDiagnosticPhase::SCHEMA, operation.mark, "",
		                relation.id.value, operation.id.value);
	}
	return result;
}

} // namespace internal
} // namespace connector
} // namespace cuac
