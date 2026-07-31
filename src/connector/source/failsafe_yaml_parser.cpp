#include "cuac/internal/connector/source/failsafe_yaml_internal.hpp"

#include <set>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {
namespace {

class Parser;

class InlineParser {
public:
	InlineParser(Parser &owner_p, const FailsafeYamlLine &line_p, std::size_t begin_p, std::size_t end_p)
	    : owner(owner_p), line(line_p), cursor(begin_p), end(end_p), flow_level(0) {
	}

	FailsafeYamlNode ParseValue(std::uint64_t depth);
	bool AtEndOrComment();
	std::size_t Cursor() const noexcept {
		return cursor;
	}

private:
	friend class Parser;

	void SkipSpaces();
	FailsafeYamlNode ParseSequence(std::uint64_t depth);
	FailsafeYamlNode ParseMapping(std::uint64_t depth);
	std::pair<std::string, SourceSpan> ParseMappingKey();
	std::pair<std::string, SourceSpan> ParseDoubleQuoted();
	std::pair<std::string, SourceSpan> ParsePlain(bool key);
	void Require(char expected, const char *message);

	Parser &owner;
	const FailsafeYamlLine &line;
	std::size_t cursor;
	std::size_t end;
	std::uint64_t flow_level;
};

class Parser {
public:
	Parser(const std::string &file_p, std::size_t byte_size_p, std::vector<FailsafeYamlLine> lines_p,
	       FailsafeYamlBudget &budget_p, PackageCancellation &cancellation_p)
	    : file(file_p), byte_size(byte_size_p), lines(std::move(lines_p)), budget(budget_p),
	      cancellation(cancellation_p), index(0) {
	}

	FailsafeYamlNode Parse() {
		Checkpoint({{0, 1, 1}, {0, 1, 1}});
		SkipBlank();
		if (index == lines.size()) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, {{0, 1, 1}, {0, 1, 1}}, "YAML document is empty");
		}
		if (lines[index].indent != 0) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(lines[index], 0, lines[index].indent),
			     "YAML root must begin at column one");
		}
		auto root = ParseBlock(0, 1);
		SkipBlank();
		if (index != lines.size()) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT,
			     PointSpan(lines[index], lines[index].indent, lines[index].text.size()),
			     "YAML document has trailing content");
		}
		return root;
	}

	void Checkpoint(const SourceSpan &span) {
		if (cancellation.IsCancellationRequested()) {
			Fail(FailsafeYamlErrorCode::CANCELLED, span, "YAML parsing was cancelled");
		}
	}

	void CheckDepth(std::uint64_t depth, const SourceSpan &span) {
		if (depth > budget.Limits().max_depth) {
			Fail(FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, span, "YAML nesting budget is exhausted");
		}
	}

	void CheckEntry(std::size_t retained, const SourceSpan &span) {
		if (retained >= budget.Limits().max_container_entries) {
			Fail(FailsafeYamlErrorCode::RESOURCE_EXHAUSTED, span, "YAML container-entry budget is exhausted");
		}
	}

	void ReserveContainer(std::uint64_t depth, const SourceSpan &span) {
		CheckDepth(depth, span);
		FailsafeYamlParserAccess::ConsumeNode(budget, file, span);
	}

	FailsafeYamlNode Scalar(std::string value, SourceSpan span, FailsafeYamlNode::ScalarStyle style) {
		return FailsafeYamlParserAccess::Scalar(std::move(value), span, style, budget, file);
	}

	FailsafeYamlNode Mapping(std::vector<FailsafeYamlParserAccess::MappingItem> entries, SourceSpan span) {
		return FailsafeYamlParserAccess::Mapping(std::move(entries), span);
	}

	FailsafeYamlNode Sequence(std::vector<FailsafeYamlNode> values, SourceSpan span) {
		return FailsafeYamlParserAccess::Sequence(std::move(values), span);
	}

	void ConsumeKey(const std::string &key, const SourceSpan &span) {
		if (key == "<<") {
			Fail(FailsafeYamlErrorCode::FORBIDDEN_SYNTAX, span, "YAML merge keys are forbidden");
		}
		FailsafeYamlParserAccess::ConsumeKey(budget, file, span);
	}

	[[noreturn]] void Fail(FailsafeYamlErrorCode code, const SourceSpan &span, const std::string &message) const {
		throw FailsafeYamlError(code, file, span, message);
	}

	const FailsafeYamlLimits &Limits() const noexcept {
		return budget.Limits();
	}

	PackageCancellation &Cancellation() noexcept {
		return cancellation;
	}

	const std::string &File() const noexcept {
		return file;
	}

private:
	friend class InlineParser;

	void SkipBlank() {
		while (index < lines.size() && lines[index].blank) {
			Checkpoint(PointSpan(lines[index], 0, lines[index].text.size()));
			index++;
		}
	}

	bool IsSequenceLine(const FailsafeYamlLine &line, std::size_t indent) const {
		return line.indent == indent && line.text.size() > indent && line.text[indent] == '-' &&
		       (line.text.size() == indent + 1 || line.text[indent + 1] == ' ');
	}

	std::size_t FindMappingColon(const FailsafeYamlLine &line, std::size_t begin) const {
		bool quoted = false;
		bool escaped = false;
		for (std::size_t position = begin; position < line.text.size(); position++) {
			const char character = line.text[position];
			if (quoted) {
				if (escaped) {
					escaped = false;
				} else if (character == '\\') {
					escaped = true;
				} else if (character == '"') {
					quoted = false;
				}
				continue;
			}
			if (character == '"') {
				quoted = true;
			} else if (character == ':' && (position + 1 == line.text.size() || line.text[position + 1] == ' ' ||
			                                line.text[position + 1] == '#' || line.text[position + 1] == '[' ||
			                                line.text[position + 1] == '{')) {
				return position;
			}
		}
		return std::string::npos;
	}

	std::pair<std::string, SourceSpan> ParseBlockKey(const FailsafeYamlLine &line, std::size_t begin,
	                                                 std::size_t colon) {
		const auto key_begin = TrimLeftSpaces(line.text, begin, colon);
		const auto key_end = TrimRightSpaces(line.text, key_begin, colon);
		if (key_begin == key_end) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, begin, colon + 1),
			     "YAML mapping key is empty");
		}
		InlineParser parser(*this, line, key_begin, key_end);
		auto parsed = parser.ParseMappingKey();
		if (!parser.AtEndOrComment()) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, parser.Cursor(), key_end),
			     "YAML mapping key is malformed");
		}
		return parsed;
	}

	FailsafeYamlNode ParseNestedOrInline(const FailsafeYamlLine &line, std::size_t value_begin,
	                                     std::size_t parent_indent, std::uint64_t depth) {
		value_begin = TrimLeftSpaces(line.text, value_begin, line.text.size());
		if (value_begin == line.text.size() || line.text[value_begin] == '#') {
			index++;
			SkipBlank();
			if (index == lines.size() || lines[index].indent <= parent_indent) {
				Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, value_begin, value_begin),
				     "YAML mapping or sequence value is missing");
			}
			return ParseBlock(lines[index].indent, depth);
		}
		InlineParser parser(*this, line, value_begin, line.text.size());
		auto value = parser.ParseValue(depth);
		if (!parser.AtEndOrComment()) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, parser.Cursor(), line.text.size()),
			     "YAML line has trailing content");
		}
		index++;
		return value;
	}

	FailsafeYamlParserAccess::MappingItem ParseMappingEntry(std::size_t indent, std::uint64_t depth,
	                                                        std::set<std::string> &keys) {
		const FailsafeYamlLine line = lines[index];
		const auto colon = FindMappingColon(line, indent);
		if (colon == std::string::npos) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, indent, line.text.size()),
			     "YAML mapping entry requires a key and colon");
		}
		auto key = ParseBlockKey(line, indent, colon);
		if (!keys.insert(key.first).second) {
			Fail(FailsafeYamlErrorCode::DUPLICATE_KEY, key.second, "YAML mapping key is duplicated");
		}
		ConsumeKey(key.first, key.second);
		auto value = ParseNestedOrInline(line, colon + 1, indent, depth + 1);
		return {std::move(key.first), key.second, std::move(value)};
	}

	FailsafeYamlNode ParseMapping(std::size_t indent, std::uint64_t depth) {
		const auto begin = PointSpan(lines[index], indent, indent + 1);
		ReserveContainer(depth, begin);
		std::vector<FailsafeYamlParserAccess::MappingItem> entries;
		std::set<std::string> keys;
		SourceSpan end = begin;
		while (index < lines.size()) {
			SkipBlank();
			if (index == lines.size() || lines[index].indent != indent || IsSequenceLine(lines[index], indent)) {
				break;
			}
			Checkpoint(PointSpan(lines[index], indent, lines[index].text.size()));
			CheckEntry(entries.size(), PointSpan(lines[index], indent, indent + 1));
			auto entry = ParseMappingEntry(indent, depth, keys);
			end = entry.value.Span();
			entries.push_back(std::move(entry));
		}
		if (entries.empty()) {
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, begin, "YAML mapping is empty or malformed");
		}
		return Mapping(std::move(entries), JoinSpan(begin, end));
	}

	FailsafeYamlNode ParseCompactMapping(const FailsafeYamlLine &first_line, std::size_t begin,
	                                     std::size_t sequence_indent, std::uint64_t depth) {
		const auto start = PointSpan(first_line, begin, begin + 1);
		ReserveContainer(depth, start);
		const auto compact_indent = begin;
		std::vector<FailsafeYamlParserAccess::MappingItem> entries;
		std::set<std::string> keys;
		SourceSpan end = start;
		while (true) {
			const FailsafeYamlLine line = lines[index];
			if (line.indent != (entries.empty() ? sequence_indent : compact_indent)) {
				break;
			}
			const auto entry_begin = entries.empty() ? begin : compact_indent;
			CheckEntry(entries.size(), PointSpan(line, entry_begin, entry_begin + 1));
			const auto colon = FindMappingColon(line, entry_begin);
			if (colon == std::string::npos) {
				break;
			}
			auto key = ParseBlockKey(line, entry_begin, colon);
			if (!keys.insert(key.first).second) {
				Fail(FailsafeYamlErrorCode::DUPLICATE_KEY, key.second, "YAML mapping key is duplicated");
			}
			ConsumeKey(key.first, key.second);
			auto value = ParseNestedOrInline(line, colon + 1, compact_indent, depth + 1);
			end = value.Span();
			entries.push_back({std::move(key.first), key.second, std::move(value)});
			SkipBlank();
			if (index == lines.size() || lines[index].indent != compact_indent ||
			    IsSequenceLine(lines[index], compact_indent)) {
				break;
			}
		}
		return Mapping(std::move(entries), JoinSpan(start, end));
	}

	FailsafeYamlNode ParseSequence(std::size_t indent, std::uint64_t depth) {
		const auto begin = PointSpan(lines[index], indent, indent + 1);
		ReserveContainer(depth, begin);
		std::vector<FailsafeYamlNode> values;
		SourceSpan end = begin;
		while (index < lines.size()) {
			SkipBlank();
			if (index == lines.size() || !IsSequenceLine(lines[index], indent)) {
				break;
			}
			const FailsafeYamlLine line = lines[index];
			Checkpoint(PointSpan(line, indent, line.text.size()));
			CheckEntry(values.size(), PointSpan(line, indent, indent + 1));
			auto value_begin = TrimLeftSpaces(line.text, indent + 1, line.text.size());
			FailsafeYamlNode value;
			if (value_begin == line.text.size() || line.text[value_begin] == '#') {
				value = ParseNestedOrInline(line, value_begin, indent, depth + 1);
			} else if (FindMappingColon(line, value_begin) != std::string::npos && line.text[value_begin] != '{' &&
			           line.text[value_begin] != '[') {
				value = ParseCompactMapping(line, value_begin, indent, depth + 1);
			} else {
				InlineParser parser(*this, line, value_begin, line.text.size());
				value = parser.ParseValue(depth + 1);
				if (!parser.AtEndOrComment()) {
					Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, parser.Cursor(), line.text.size()),
					     "YAML line has trailing content");
				}
				index++;
			}
			end = value.Span();
			values.push_back(std::move(value));
		}
		return Sequence(std::move(values), JoinSpan(begin, end));
	}

	FailsafeYamlNode ParseBlock(std::size_t indent, std::uint64_t depth) {
		SkipBlank();
		if (index == lines.size() || lines[index].indent != indent) {
			const auto last_line = lines.empty() ? 1 : lines.back().number;
			const auto span = index == lines.size()
			                      ? SourceSpan {{byte_size, last_line, 1}, {byte_size, last_line, 1}}
			                      : PointSpan(lines[index], lines[index].indent, lines[index].indent + 1);
			Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, span, "YAML indentation is inconsistent");
		}
		return IsSequenceLine(lines[index], indent) ? ParseSequence(indent, depth) : ParseMapping(indent, depth);
	}

	const std::string &file;
	std::size_t byte_size;
	std::vector<FailsafeYamlLine> lines;
	FailsafeYamlBudget &budget;
	PackageCancellation &cancellation;
	std::size_t index;
};

void InlineParser::SkipSpaces() {
	while (cursor < end && line.text[cursor] == ' ') {
		cursor++;
	}
}

void InlineParser::Require(char expected, const char *message) {
	if (cursor >= end || line.text[cursor] != expected) {
		owner.Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor, cursor), message);
	}
	cursor++;
}

std::pair<std::string, SourceSpan> InlineParser::ParseDoubleQuoted() {
	auto scalar = ParseDoubleQuotedScalar(owner.File(), line, cursor, end, owner.Limits(), owner.Cancellation());
	cursor = scalar.next;
	return {std::move(scalar.value), scalar.span};
}

std::pair<std::string, SourceSpan> InlineParser::ParsePlain(bool key) {
	auto scalar =
	    ParsePlainScalar(owner.File(), line, cursor, end, key, flow_level > 0, owner.Limits(), owner.Cancellation());
	cursor = scalar.next;
	return {std::move(scalar.value), scalar.span};
}

std::pair<std::string, SourceSpan> InlineParser::ParseMappingKey() {
	SkipSpaces();
	return cursor < end && line.text[cursor] == '"' ? ParseDoubleQuoted() : ParsePlain(true);
}

FailsafeYamlNode InlineParser::ParseSequence(std::uint64_t depth) {
	const auto begin = cursor;
	owner.ReserveContainer(depth, PointSpan(line, begin, begin + 1));
	Require('[', "flow sequence is malformed");
	flow_level++;
	std::vector<FailsafeYamlNode> values;
	SkipSpaces();
	if (cursor < end && line.text[cursor] == ']') {
		cursor++;
		flow_level--;
		return owner.Sequence(std::move(values), PointSpan(line, begin, cursor));
	}
	while (true) {
		owner.CheckEntry(values.size(), PointSpan(line, cursor, cursor + 1));
		values.push_back(ParseValue(depth + 1));
		SkipSpaces();
		if (cursor < end && line.text[cursor] == ']') {
			cursor++;
			break;
		}
		Require(',', "flow sequence requires a comma or closing bracket");
		SkipSpaces();
		if (cursor < end && line.text[cursor] == ']') {
			owner.Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor, cursor + 1),
			           "flow sequence cannot have a trailing comma");
		}
	}
	flow_level--;
	return owner.Sequence(std::move(values), PointSpan(line, begin, cursor));
}

FailsafeYamlNode InlineParser::ParseMapping(std::uint64_t depth) {
	const auto begin = cursor;
	owner.ReserveContainer(depth, PointSpan(line, begin, begin + 1));
	Require('{', "flow mapping is malformed");
	flow_level++;
	std::vector<FailsafeYamlParserAccess::MappingItem> entries;
	std::set<std::string> keys;
	SkipSpaces();
	if (cursor < end && line.text[cursor] == '}') {
		cursor++;
		flow_level--;
		return owner.Mapping(std::move(entries), PointSpan(line, begin, cursor));
	}
	while (true) {
		owner.CheckEntry(entries.size(), PointSpan(line, cursor, cursor + 1));
		auto key = ParseMappingKey();
		if (!keys.insert(key.first).second) {
			owner.Fail(FailsafeYamlErrorCode::DUPLICATE_KEY, key.second, "YAML mapping key is duplicated");
		}
		owner.ConsumeKey(key.first, key.second);
		SkipSpaces();
		Require(':', "flow mapping requires a colon");
		SkipSpaces();
		auto value = ParseValue(depth + 1);
		entries.push_back({std::move(key.first), key.second, std::move(value)});
		SkipSpaces();
		if (cursor < end && line.text[cursor] == '}') {
			cursor++;
			break;
		}
		Require(',', "flow mapping requires a comma or closing brace");
		SkipSpaces();
		if (cursor < end && line.text[cursor] == '}') {
			owner.Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor, cursor + 1),
			           "flow mapping cannot have a trailing comma");
		}
	}
	flow_level--;
	return owner.Mapping(std::move(entries), PointSpan(line, begin, cursor));
}

FailsafeYamlNode InlineParser::ParseValue(std::uint64_t depth) {
	SkipSpaces();
	if (cursor >= end || line.text[cursor] == '#') {
		owner.Fail(FailsafeYamlErrorCode::MALFORMED_DOCUMENT, PointSpan(line, cursor, cursor), "YAML value is missing");
	}
	if (line.text[cursor] == '[') {
		return ParseSequence(depth);
	}
	if (line.text[cursor] == '{') {
		return ParseMapping(depth);
	}
	if (line.text[cursor] == '"') {
		auto value = ParseDoubleQuoted();
		return owner.Scalar(std::move(value.first), value.second, FailsafeYamlNode::ScalarStyle::DOUBLE_QUOTED);
	}
	auto value = ParsePlain(false);
	return owner.Scalar(std::move(value.first), value.second, FailsafeYamlNode::ScalarStyle::PLAIN);
}

bool InlineParser::AtEndOrComment() {
	SkipSpaces();
	return cursor == end || line.text[cursor] == '#';
}

} // namespace

FailsafeYamlNode ParseFailsafeYamlStructure(const std::string &file, std::size_t byte_size,
                                            std::vector<FailsafeYamlLine> lines, FailsafeYamlBudget &budget,
                                            PackageCancellation &cancellation) {
	Parser parser(file, byte_size, std::move(lines), budget, cancellation);
	return parser.Parse();
}

} // namespace internal
} // namespace connector
} // namespace cuac
