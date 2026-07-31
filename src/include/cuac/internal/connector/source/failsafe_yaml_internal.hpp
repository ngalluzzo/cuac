#pragma once

#include "cuac/internal/connector/source/failsafe_yaml.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {

// Private construction access named by the public node/budget friendship. It
// is deliberately unavailable outside the Connector package target.
class FailsafeYamlParserAccess {
public:
	struct MappingItem {
		std::string key;
		SourceSpan key_span;
		FailsafeYamlNode value;
	};

	static void ConsumeNode(FailsafeYamlBudget &budget, const std::string &file, const SourceSpan &span);
	static void ConsumeKey(FailsafeYamlBudget &budget, const std::string &file, const SourceSpan &span);
	static FailsafeYamlNode Scalar(std::string value, SourceSpan span, FailsafeYamlNode::ScalarStyle style,
	                               FailsafeYamlBudget &budget, const std::string &file);
	static FailsafeYamlNode Mapping(std::vector<MappingItem> entries, SourceSpan span);
	static FailsafeYamlNode Sequence(std::vector<FailsafeYamlNode> values, SourceSpan span);
};

namespace internal {

struct FailsafeYamlLine {
	std::string text;
	std::uint64_t offset;
	std::uint64_t number;
	std::size_t indent;
	bool blank;
};

struct ParsedYamlScalar {
	std::string value;
	SourceSpan span;
	std::size_t next;
};

SourceSpan PointSpan(const FailsafeYamlLine &line, std::size_t begin, std::size_t end);
SourceSpan JoinSpan(const SourceSpan &begin, const SourceSpan &end);
std::size_t TrimRightSpaces(const std::string &value, std::size_t begin, std::size_t end);
std::size_t TrimLeftSpaces(const std::string &value, std::size_t begin, std::size_t end);

// Lexical custody validates UTF-8 and forbidden document/indentation forms
// before structural parsing retains a node.
std::vector<FailsafeYamlLine> LexFailsafeYaml(const std::string &file, const std::string &bytes,
                                              PackageCancellation &cancellation);

// Scalar decoding is separate from block/flow structure so JSON escape and
// failsafe-token policy can evolve without changing tree assembly.
ParsedYamlScalar ParseDoubleQuotedScalar(const std::string &file, const FailsafeYamlLine &line, std::size_t begin,
                                         std::size_t end, const FailsafeYamlLimits &limits,
                                         PackageCancellation &cancellation);
ParsedYamlScalar ParsePlainScalar(const std::string &file, const FailsafeYamlLine &line, std::size_t begin,
                                  std::size_t end, bool mapping_key, bool flow_context,
                                  const FailsafeYamlLimits &limits, PackageCancellation &cancellation);

FailsafeYamlNode ParseFailsafeYamlStructure(const std::string &file, std::size_t byte_size,
                                            std::vector<FailsafeYamlLine> lines, FailsafeYamlBudget &budget,
                                            PackageCancellation &cancellation);

} // namespace internal
} // namespace connector
} // namespace cuac
