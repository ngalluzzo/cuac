#pragma once

#include "cuac/internal/connector/compiler/package_compiler.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {

struct SourceMark {
	std::string file;
	SourceSpan span;
	std::string yaml_path;
};

class PackageDiagnosticSink {
public:
	explicit PackageDiagnosticSink(std::uint64_t maximum);

	void Add(PackageDiagnosticCode code, PackageDiagnosticPhase phase, const SourceMark &mark,
	         const std::string &connector = "", const std::string &relation = "", const std::string &operation = "",
	         const SourceMark *related = nullptr);
	bool Empty() const noexcept;
	std::uint64_t Revision() const noexcept;
	std::vector<PackageDiagnostic> Finish();

private:
	std::uint64_t maximum;
	std::uint64_t revision;
	bool overflowed;
	std::vector<PackageDiagnostic> diagnostics;
};

struct LocatedText {
	std::string value;
	SourceMark mark;
	FailsafeYamlNode::ScalarStyle style;
};

} // namespace internal
} // namespace connector
} // namespace cuac
