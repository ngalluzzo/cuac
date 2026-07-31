#pragma once

#include "cuac/internal/connector/compiler/package_compiler_internal.hpp"

#include <initializer_list>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

class SchemaReader {
public:
	SchemaReader(const std::string &file, const FailsafeYamlNode &node, std::string path,
	             PackageDiagnosticSink &diagnostics);

	bool RequireMapping(std::initializer_list<const char *> allowed,
	                    std::initializer_list<const char *> required) const;
	const FailsafeYamlNode *Field(const std::string &name) const;
	LocatedText Text(const std::string &name, bool required = true) const;
	std::vector<LocatedText> TextSequence(const std::string &name, std::size_t minimum, std::size_t maximum) const;
	const FailsafeYamlNode *Sequence(const std::string &name, std::size_t minimum, std::size_t maximum) const;
	SchemaReader Child(const std::string &name) const;
	SchemaReader Child(const FailsafeYamlNode &child, const std::string &suffix) const;
	SourceMark Mark() const;
	SourceMark FieldMark(const std::string &name) const;
	const std::string &File() const;
	const std::string &Path() const;
	PackageDiagnosticSink &Diagnostics() const;

private:
	const std::string &file;
	const FailsafeYamlNode &node;
	std::string path;
	PackageDiagnosticSink &diagnostics;
};

bool IsIdentifier(const std::string &value);
bool IsGraphqlName(const std::string &value);
bool IsCanonicalUnsigned(const LocatedText &value, std::uint64_t &parsed);
bool IsCanonicalSigned(const LocatedText &value, std::int64_t &parsed);
bool IsPlainBoolean(const LocatedText &value, bool &parsed);
// RFC 0020: a plain-style JSON-number-shaped scalar (optional '-', digits
// with no unnecessary leading zero, optional '.' fraction, optional
// exponent), converted via strtod and rejecting only true overflow
// (HUGE_VAL); -0.0 is normalized to 0.0. Author-declared "nan"/"inf"/
// "infinity" text is rejected by the grammar check before strtod ever sees
// it, since those are not JSON-number-shaped.
bool IsCanonicalDouble(const LocatedText &value, double &parsed);
bool IsExtractor(const std::string &value, bool collection, std::vector<std::string> *segments = nullptr);
bool IsFixedPath(const std::string &value);
bool IsHeaderName(const std::string &value);
bool IsRateLimitHeaderName(const std::string &value);
bool IsRateLimitOperationFamily(const std::string &value);
bool IsHeaderValue(const std::string &value);
bool IsQueryName(const std::string &value);
bool IsCanonicalHost(const std::string &value);

} // namespace internal
} // namespace connector
} // namespace cuac
