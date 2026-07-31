#include "cuac/internal/connector/compiler/package_relation_schema_parts.hpp"

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

HeaderDeclaration DecodeHeader(const SchemaReader &reader) {
	HeaderDeclaration header;
	reader.RequireMapping({"name", "value"}, {"name", "value"});
	header.name = reader.Text("name");
	header.value = reader.Text("value");
	header.mark = reader.Mark();
	if (!IsHeaderName(header.name.value) || !IsHeaderValue(header.value.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
		                         header.mark);
	}
	return header;
}

} // namespace

OriginDeclaration DecodeHttpOrigin(const SchemaReader &reader) {
	OriginDeclaration origin;
	reader.RequireMapping({"scheme", "host", "port"}, {"scheme", "host", "port"});
	origin.scheme = reader.Text("scheme");
	origin.host = reader.Text("host");
	origin.port = reader.Text("port");
	origin.mark = reader.Mark();
	RequireValue(origin.scheme, "https", PackageDiagnosticCode::UNSUPPORTED_DECLARATION, PackageDiagnosticPhase::SCHEMA,
	             reader.Diagnostics());
	std::uint64_t port = 0;
	if (!IsCanonicalHost(origin.host.value)) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, origin.host.mark);
	}
	if (!IsCanonicalUnsigned(origin.port, port) || port > 65535) {
		reader.Diagnostics().Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, origin.port.mark);
	}
	return origin;
}

std::vector<HeaderDeclaration> DecodeHttpHeaders(const SchemaReader &reader) {
	std::vector<HeaderDeclaration> headers;
	const auto *sequence = reader.Sequence("headers", 0, 32);
	if (sequence == nullptr) {
		return headers;
	}
	headers.reserve(sequence->Size());
	for (std::size_t index = 0; index < sequence->Size(); index++) {
		headers.push_back(
		    DecodeHeader(reader.Child(sequence->SequenceValue(index), ".headers[" + std::to_string(index) + "]")));
	}
	return headers;
}

} // namespace internal
} // namespace connector
} // namespace cuac
