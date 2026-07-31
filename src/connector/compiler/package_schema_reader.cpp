#include "cuac/internal/connector/compiler/package_schema_helpers.hpp"

namespace cuac {
namespace connector {
namespace internal {

namespace {

bool Contains(std::initializer_list<const char *> values, const std::string &expected) {
	for (const auto *value : values) {
		if (expected == value) {
			return true;
		}
	}
	return false;
}

SourceSpan Anchor(const FailsafeYamlNode &node) {
	if (node.Type() == FailsafeYamlNode::Kind::MAPPING && node.Size() > 0) {
		return node.MappingKeySpan(0);
	}
	return node.Span();
}

} // namespace

SchemaReader::SchemaReader(const std::string &file_p, const FailsafeYamlNode &node_p, std::string path_p,
                           PackageDiagnosticSink &diagnostics_p)
    : file(file_p), node(node_p), path(std::move(path_p)), diagnostics(diagnostics_p) {
}

bool SchemaReader::RequireMapping(std::initializer_list<const char *> allowed,
                                  std::initializer_list<const char *> required) const {
	if (node.Type() != FailsafeYamlNode::Kind::MAPPING) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, Mark());
		return false;
	}
	for (std::size_t index = 0; index < node.Size(); index++) {
		if (!Contains(allowed, node.MappingKey(index))) {
			SourceMark mark = {file, node.MappingKeySpan(index), path + "." + node.MappingKey(index)};
			diagnostics.Add(PackageDiagnosticCode::UNKNOWN_FIELD, PackageDiagnosticPhase::SCHEMA, mark);
		}
	}
	for (const auto *name : required) {
		if (node.Find(name) == nullptr) {
			SourceMark mark = {file, Anchor(node), path + "." + name};
			diagnostics.Add(PackageDiagnosticCode::MISSING_FIELD, PackageDiagnosticPhase::SCHEMA, mark);
		}
	}
	return true;
}

const FailsafeYamlNode *SchemaReader::Field(const std::string &name) const {
	return node.Type() == FailsafeYamlNode::Kind::MAPPING ? node.Find(name) : nullptr;
}

LocatedText SchemaReader::Text(const std::string &name, bool required) const {
	const auto *value = Field(name);
	if (value == nullptr) {
		return {"", FieldMark(name), FailsafeYamlNode::ScalarStyle::PLAIN};
	}
	if (value->Type() != FailsafeYamlNode::Kind::SCALAR) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, FieldMark(name));
		return {"", FieldMark(name), FailsafeYamlNode::ScalarStyle::PLAIN};
	}
	(void)required;
	return {value->Scalar(), FieldMark(name), value->Style()};
}

std::vector<LocatedText> SchemaReader::TextSequence(const std::string &name, std::size_t minimum,
                                                    std::size_t maximum) const {
	std::vector<LocatedText> result;
	const auto *sequence = Sequence(name, minimum, maximum);
	if (sequence == nullptr) {
		return result;
	}
	result.reserve(sequence->Size());
	for (std::size_t index = 0; index < sequence->Size(); index++) {
		const auto &value = sequence->SequenceValue(index);
		SourceMark mark = {file, value.Span(), path + "." + name + "[" + std::to_string(index) + "]"};
		if (value.Type() != FailsafeYamlNode::Kind::SCALAR) {
			diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, mark);
			continue;
		}
		result.push_back({value.Scalar(), mark, value.Style()});
	}
	return result;
}

const FailsafeYamlNode *SchemaReader::Sequence(const std::string &name, std::size_t minimum,
                                               std::size_t maximum) const {
	const auto *value = Field(name);
	if (value == nullptr) {
		return nullptr;
	}
	if (value->Type() != FailsafeYamlNode::Kind::SEQUENCE) {
		diagnostics.Add(PackageDiagnosticCode::INVALID_TYPE, PackageDiagnosticPhase::SCHEMA, FieldMark(name));
		return nullptr;
	}
	if (value->Size() < minimum || value->Size() > maximum) {
		diagnostics.Add(value->Size() > maximum ? PackageDiagnosticCode::RESOURCE_EXHAUSTED
		                                        : PackageDiagnosticCode::INVALID_TYPE,
		                PackageDiagnosticPhase::SCHEMA, FieldMark(name));
		return nullptr;
	}
	return value;
}

SchemaReader SchemaReader::Child(const std::string &name) const {
	const auto *child = Field(name);
	return SchemaReader(file, child == nullptr ? node : *child, path + "." + name, diagnostics);
}

SchemaReader SchemaReader::Child(const FailsafeYamlNode &child, const std::string &suffix) const {
	return SchemaReader(file, child, path + suffix, diagnostics);
}

SourceMark SchemaReader::Mark() const {
	return {file, node.Span(), path};
}

SourceMark SchemaReader::FieldMark(const std::string &name) const {
	const auto *value = Field(name);
	return {file, value == nullptr ? Anchor(node) : value->Span(), path + "." + name};
}

const std::string &SchemaReader::File() const {
	return file;
}

const std::string &SchemaReader::Path() const {
	return path;
}

PackageDiagnosticSink &SchemaReader::Diagnostics() const {
	return diagnostics;
}

} // namespace internal
} // namespace connector
} // namespace cuac
