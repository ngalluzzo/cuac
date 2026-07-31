#pragma once

#include "cuac/internal/connector/compiler/package_cancellation.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>

namespace cuac {
namespace connector {

struct SourcePosition {
	std::uint64_t byte_offset;
	std::uint64_t line;
	std::uint64_t column;
};

struct SourceSpan {
	SourcePosition begin;
	SourcePosition end;
};

enum class FailsafeYamlErrorCode : std::uint8_t {
	CANCELLED,
	INVALID_ENCODING,
	FORBIDDEN_SYNTAX,
	MALFORMED_DOCUMENT,
	DUPLICATE_KEY,
	RESOURCE_EXHAUSTED
};

// Safe Connector-owned lexical failure. File is an already validated package-
// relative path; message never includes source bytes or an absolute package
// root. Schema and field-role validation deliberately occur above this layer.
class FailsafeYamlError : public std::exception {
public:
	FailsafeYamlError(FailsafeYamlErrorCode code, std::string file, SourceSpan span, std::string safe_message);

	const char *what() const noexcept override;
	FailsafeYamlErrorCode Code() const noexcept;
	const std::string &File() const noexcept;
	const SourceSpan &Span() const noexcept;

private:
	FailsafeYamlErrorCode code;
	std::string file;
	SourceSpan span;
	std::string safe_message;
};

struct FailsafeYamlLimits {
	std::uint64_t max_depth;
	std::uint64_t max_nodes;
	std::uint64_t max_scalar_bytes;
	std::uint64_t max_container_entries;

	static FailsafeYamlLimits V1();
};

// Mutable accounting object shared by every semantic document in one immutable
// package snapshot. Limits are effective spec/host ceilings; zero is invalid.
// A failed parse consumes the accounting object and the enclosing compilation
// must discard it with the candidate.
class FailsafeYamlBudget {
public:
	explicit FailsafeYamlBudget(FailsafeYamlLimits limits);

	const FailsafeYamlLimits &Limits() const noexcept;
	std::uint64_t NodesConsumed() const noexcept;

private:
	FailsafeYamlLimits limits;
	std::uint64_t nodes_consumed;

	friend class FailsafeYamlParserAccess;
};

// Immutable failsafe-schema tree. Scalars are decoded UTF-8 text only; no YAML
// implicit typing occurs. Mapping order and sequence order match source order.
// Copies share immutable storage and remain valid independently of the input
// byte buffer.
class FailsafeYamlNode {
public:
	enum class Kind : std::uint8_t { SCALAR, MAPPING, SEQUENCE };
	enum class ScalarStyle : std::uint8_t { PLAIN, DOUBLE_QUOTED };

	FailsafeYamlNode();
	Kind Type() const;
	const SourceSpan &Span() const;
	const std::string &Scalar() const;
	ScalarStyle Style() const;
	std::size_t Size() const;
	const std::string &MappingKey(std::size_t index) const;
	const SourceSpan &MappingKeySpan(std::size_t index) const;
	const FailsafeYamlNode &MappingValue(std::size_t index) const;
	const FailsafeYamlNode &SequenceValue(std::size_t index) const;
	const FailsafeYamlNode *Find(const std::string &key) const;

private:
	class Data;
	explicit FailsafeYamlNode(std::shared_ptr<const Data> data);
	std::shared_ptr<const Data> data;

	friend class FailsafeYamlParserAccess;
};

// Parses one UTF-8 YAML 1.2 document under the failsafe lexical contract. The
// supported collections are block and single-line flow mappings/sequences;
// plain and JSON-escaped double-quoted scalars are the only scalar forms.
// Cancellation is checked before validation, at every logical line, and while
// decoding bounded scalars. No network, credential, filesystem, or schema
// authority is acquired here.
FailsafeYamlNode ParseFailsafeYaml(const std::string &package_relative_file, const std::string &bytes,
                                   FailsafeYamlBudget &budget, PackageCancellation &cancellation);

} // namespace connector
} // namespace cuac
