#include "cuac/query/scan_request.hpp"

#include <locale>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "cuac/semantics/cache_policy.hpp"

namespace cuac {

namespace {

const char *ExplicitInputValueKindName(ExplicitInputValueKind kind) {
	switch (kind) {
	case ExplicitInputValueKind::BOOLEAN:
		return "boolean";
	case ExplicitInputValueKind::BIGINT:
		return "bigint";
	case ExplicitInputValueKind::VARCHAR:
		return "varchar";
	case ExplicitInputValueKind::DOUBLE:
		return "double";
	case ExplicitInputValueKind::TIMESTAMPTZ:
		return "timestamptz";
	}
	throw std::logic_error("explicit input contains an unknown value kind");
}

std::string HexEncode(const std::string &value) {
	static const char HEX_DIGITS[] = "0123456789abcdef";
	std::string result;
	result.reserve(value.size() * 2);
	for (const char character : value) {
		const auto byte = static_cast<unsigned char>(character);
		result.push_back(HEX_DIGITS[byte >> 4]);
		result.push_back(HEX_DIGITS[byte & 0x0f]);
	}
	return result;
}

const char *RetainedPredicateScopeName(RetainedPredicateScope scope) {
	switch (scope) {
	case RetainedPredicateScope::UNRESTRICTED:
		return "unrestricted";
	case RetainedPredicateScope::REQUESTED_PREDICATE:
		return "requested_predicate";
	case RetainedPredicateScope::COMPLETE_DUCKDB_FILTER:
		return "complete_duckdb_filter";
	}
	throw std::logic_error("scan request contains an unknown retained-predicate scope");
}

} // namespace

ExplicitInput::ExplicitInput(std::string identifier_p, ExplicitInputValueKind kind_p, bool is_null_p,
                             bool boolean_value_p, std::int64_t bigint_value_p, std::string varchar_value_p,
                             double double_value_p, std::int64_t timestamptz_microseconds_p)
    : identifier(std::move(identifier_p)), kind(kind_p), is_null(is_null_p), boolean_value(boolean_value_p),
      bigint_value(bigint_value_p), varchar_value(std::move(varchar_value_p)), double_value(double_value_p),
      timestamptz_microseconds(timestamptz_microseconds_p) {
	if (identifier.empty()) {
		throw std::invalid_argument("explicit input identifier must not be empty");
	}
	(void)ExplicitInputValueKindName(kind);
}

ExplicitInput ExplicitInput::Null(std::string identifier, ExplicitInputValueKind kind) {
	return ExplicitInput(std::move(identifier), kind, true, false, 0, std::string(), 0.0);
}

ExplicitInput ExplicitInput::Boolean(std::string identifier, bool value) {
	return ExplicitInput(std::move(identifier), ExplicitInputValueKind::BOOLEAN, false, value, 0, std::string(), 0.0);
}

ExplicitInput ExplicitInput::BigInt(std::string identifier, std::int64_t value) {
	return ExplicitInput(std::move(identifier), ExplicitInputValueKind::BIGINT, false, false, value, std::string(),
	                     0.0);
}

ExplicitInput ExplicitInput::Varchar(std::string identifier, std::string value) {
	return ExplicitInput(std::move(identifier), ExplicitInputValueKind::VARCHAR, false, false, 0, std::move(value),
	                     0.0);
}

ExplicitInput ExplicitInput::Double(std::string identifier, double value) {
	// RFC 0020: -0.0 is normalized to 0.0 so every consumer sees one canonical zero.
	return ExplicitInput(std::move(identifier), ExplicitInputValueKind::DOUBLE, false, false, 0, std::string(),
	                     value == 0.0 ? 0.0 : value);
}

ExplicitInput ExplicitInput::Timestamptz(std::string identifier, std::int64_t microseconds) {
	if (!IsTimestamptzMicroseconds(microseconds)) {
		throw std::invalid_argument("explicit TIMESTAMPTZ input is outside the CUAC profile");
	}
	return ExplicitInput(std::move(identifier), ExplicitInputValueKind::TIMESTAMPTZ, false, false, 0, std::string(),
	                     0.0, microseconds);
}

const std::string &ExplicitInput::Identifier() const noexcept {
	return identifier;
}

ExplicitInputValueKind ExplicitInput::Kind() const noexcept {
	return kind;
}

bool ExplicitInput::IsNull() const noexcept {
	return is_null;
}

bool ExplicitInput::BooleanValue() const {
	if (is_null || kind != ExplicitInputValueKind::BOOLEAN) {
		throw std::logic_error("explicit input does not contain a BOOLEAN value");
	}
	return boolean_value;
}

std::int64_t ExplicitInput::BigIntValue() const {
	if (is_null || kind != ExplicitInputValueKind::BIGINT) {
		throw std::logic_error("explicit input does not contain a BIGINT value");
	}
	return bigint_value;
}

const std::string &ExplicitInput::VarcharValue() const {
	if (is_null || kind != ExplicitInputValueKind::VARCHAR) {
		throw std::logic_error("explicit input does not contain a VARCHAR value");
	}
	return varchar_value;
}

double ExplicitInput::DoubleValue() const {
	if (is_null || kind != ExplicitInputValueKind::DOUBLE) {
		throw std::logic_error("explicit input does not contain a DOUBLE value");
	}
	return double_value;
}

std::int64_t ExplicitInput::TimestamptzMicroseconds() const {
	if (is_null || kind != ExplicitInputValueKind::TIMESTAMPTZ) {
		throw std::logic_error("explicit input does not contain a TIMESTAMPTZ value");
	}
	return timestamptz_microseconds;
}

bool ExplicitInput::operator==(const ExplicitInput &other) const noexcept {
	return identifier == other.identifier && kind == other.kind && is_null == other.is_null &&
	       boolean_value == other.boolean_value && bigint_value == other.bigint_value &&
	       varchar_value == other.varchar_value && double_value == other.double_value &&
	       timestamptz_microseconds == other.timestamptz_microseconds;
}

bool ExplicitInput::operator!=(const ExplicitInput &other) const noexcept {
	return !(*this == other);
}

std::string ExplicitInput::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	// 17 significant digits so a DOUBLE value round-trips through this snapshot,
	// matching the wire-encoding precision used elsewhere for RFC 0020 (see
	// EncodeCanonicalDouble in protocol_operation_declaration.cpp).
	result.precision(17);
	result << "input[id=hex:" << HexEncode(identifier) << ",kind=" << ExplicitInputValueKindName(kind) << ",value=";
	if (is_null) {
		result << "null";
	} else {
		switch (kind) {
		case ExplicitInputValueKind::BOOLEAN:
			result << (boolean_value ? "true" : "false");
			break;
		case ExplicitInputValueKind::BIGINT:
			result << bigint_value;
			break;
		case ExplicitInputValueKind::VARCHAR:
			result << "hex:" << HexEncode(varchar_value);
			break;
		case ExplicitInputValueKind::DOUBLE:
			result << double_value;
			break;
		case ExplicitInputValueKind::TIMESTAMPTZ:
			result << CanonicalTimestamptz(timestamptz_microseconds);
			break;
		}
	}
	result << ']';
	return result.str();
}

ExplicitInputs::ExplicitInputs() : values() {
}

ExplicitInputs::ExplicitInputs(std::vector<ExplicitInput> values_p) : values(std::move(values_p)) {
	std::set<std::string> identifiers;
	for (const auto &value : values) {
		if (value.Identifier().empty()) {
			throw std::invalid_argument("explicit input identifier must not be empty");
		}
		if (!identifiers.insert(value.Identifier()).second) {
			throw std::invalid_argument("explicit input identifiers must be unique");
		}
	}
}

ExplicitInputs::ExplicitInputs(std::initializer_list<ExplicitInput> values_p)
    : ExplicitInputs(std::vector<ExplicitInput>(values_p)) {
}

bool ExplicitInputs::empty() const noexcept {
	return values.empty();
}

std::size_t ExplicitInputs::size() const noexcept {
	return values.size();
}

const ExplicitInput &ExplicitInputs::At(std::size_t index) const {
	return values.at(index);
}

const ExplicitInput *ExplicitInputs::Find(const std::string &exact_identifier) const noexcept {
	for (const auto &value : values) {
		if (value.Identifier() == exact_identifier) {
			return &value;
		}
	}
	return nullptr;
}

const std::vector<ExplicitInput> &ExplicitInputs::Values() const noexcept {
	return values;
}

ExplicitInputs::const_iterator ExplicitInputs::begin() const noexcept {
	return values.begin();
}

ExplicitInputs::const_iterator ExplicitInputs::end() const noexcept {
	return values.end();
}

bool ExplicitInputs::operator==(const ExplicitInputs &other) const noexcept {
	return values == other.values;
}

bool ExplicitInputs::operator!=(const ExplicitInputs &other) const noexcept {
	return !(*this == other);
}

std::string ExplicitInputs::Snapshot() const {
	std::ostringstream result;
	result << '[';
	for (std::size_t index = 0; index < values.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << values[index].Snapshot();
	}
	result << ']';
	return result.str();
}

LogicalSecretReference::LogicalSecretReference() : exact_duckdb_secret_name() {
}

LogicalSecretReference::LogicalSecretReference(std::string exact_duckdb_secret_name_p)
    : exact_duckdb_secret_name(std::move(exact_duckdb_secret_name_p)) {
}

LogicalSecretReference LogicalSecretReference::Named(std::string exact_duckdb_secret_name) {
	if (exact_duckdb_secret_name.empty()) {
		throw std::invalid_argument("logical secret reference name must not be empty");
	}
	return LogicalSecretReference(std::move(exact_duckdb_secret_name));
}

bool LogicalSecretReference::IsPresent() const noexcept {
	return !exact_duckdb_secret_name.empty();
}

const std::string &LogicalSecretReference::Name() const {
	if (!IsPresent()) {
		throw std::logic_error("logical secret reference is absent");
	}
	return exact_duckdb_secret_name;
}

std::string LogicalSecretReference::Snapshot() const {
	if (!IsPresent()) {
		return "none";
	}
	static const char HEX_DIGITS[] = "0123456789abcdef";
	std::string result = "named-hex:";
	result.reserve(result.size() + exact_duckdb_secret_name.size() * 2);
	for (const char character : exact_duckdb_secret_name) {
		const auto byte = static_cast<unsigned char>(character);
		result.push_back(HEX_DIGITS[byte >> 4]);
		result.push_back(HEX_DIGITS[byte & 0x0f]);
	}
	return result;
}

bool AdapterCapabilities::HasConservativeRelationalProfile() const {
	return !projection && !filter && !selective_predicate && !retains_predicate && !ordering && !limit && !offset &&
	       !progress && cancellation;
}

ScanGenerationIdentity::ScanGenerationIdentity(std::string spec_identifier_p, std::string connector_id_p,
                                               std::string package_version_p, std::string package_digest_p)
    : spec_identifier(std::move(spec_identifier_p)), connector_id(std::move(connector_id_p)),
      package_version(std::move(package_version_p)), package_digest(std::move(package_digest_p)) {
}

bool ScanGenerationIdentity::operator==(const ScanGenerationIdentity &other) const noexcept {
	return spec_identifier == other.spec_identifier && connector_id == other.connector_id &&
	       package_version == other.package_version && package_digest == other.package_digest;
}

bool ScanGenerationIdentity::operator!=(const ScanGenerationIdentity &other) const noexcept {
	return !(*this == other);
}

std::string ScanGenerationIdentity::Snapshot() const {
	std::ostringstream result;
	result.imbue(std::locale::classic());
	result << "package,spec_identifier=hex:" << HexEncode(spec_identifier)
	       << ",connector_id=hex:" << HexEncode(connector_id) << ",version=hex:" << HexEncode(package_version)
	       << ",digest=hex:" << HexEncode(package_digest);
	return result.str();
}

ScanGenerationIdentity BuildPackageScanGenerationIdentity(const CompiledPackageIdentity &identity) {
	return {identity.SpecIdentifier(), identity.ConnectorId(), identity.PackageVersion(), identity.PackageDigest()};
}

std::string ScanRequest::Snapshot() const {
	std::ostringstream result;
	result << "connector=" << connector_name << ";relation=" << relation_name
	       << ";inputs=" << explicit_inputs.Snapshot() << ";projection=";
	for (std::size_t index = 0; index < projected_columns.size(); index++) {
		if (index > 0) {
			result << ',';
		}
		result << projected_columns[index];
	}
	result << ";requested-predicate=" << requested_predicate.Snapshot()
	       << ";retained-predicate-scope=" << RetainedPredicateScopeName(retained_predicate_scope)
	       << ";ordering=" << (orderings.empty() ? "[]" : "unexpected") << ";limit=" << (has_limit ? "set" : "unset")
	       << ";offset=" << (has_offset ? "set" : "unset")
	       << ";capabilities=projection:" << (capabilities.projection ? "available" : "unavailable")
	       << ",filter:" << (capabilities.filter ? "available" : "unavailable")
	       << ",selective-predicate:" << (capabilities.selective_predicate ? "available" : "unavailable")
	       << ",retains-predicate:" << (capabilities.retains_predicate ? "verified" : "unavailable")
	       << ",ordering:" << (capabilities.ordering ? "available" : "unavailable")
	       << ",limit:" << (capabilities.limit ? "available" : "unavailable")
	       << ",offset:" << (capabilities.offset ? "available" : "unavailable")
	       << ",progress:" << (capabilities.progress ? "available" : "unavailable")
	       << ",cancellation:" << (capabilities.cancellation ? "verified" : "unavailable")
	       << ",secret-manager:" << (capabilities.secret_manager ? "available" : "unavailable")
	       << ";secret-reference=" << secret_reference.Snapshot() << ";generation=" << generation_identity.Snapshot()
	       << ";" << freshness_policy.Snapshot();
	return result.str();
}

ScanRequest BuildPackageScanRequest(const CompiledPackageIdentity &identity,
                                    const CompiledRegistrationRelation &relation, ExplicitInputs explicit_inputs,
                                    LogicalSecretReference secret_reference) {
	const auto authentication = relation.Authentication();
	if (authentication == CompiledRegistrationAuthentication::ANONYMOUS && secret_reference.IsPresent()) {
		throw std::invalid_argument("anonymous relation does not accept a logical secret reference");
	}
	if (authentication == CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED &&
	    !secret_reference.IsPresent()) {
		throw std::invalid_argument("authenticated relation requires a logical secret reference");
	}
	if (authentication != CompiledRegistrationAuthentication::ANONYMOUS &&
	    authentication != CompiledRegistrationAuthentication::LOGICAL_SECRET_REQUIRED) {
		throw std::invalid_argument("selected relation has an unsupported authentication shape");
	}

	ScanRequest result;
	result.connector_name = identity.ConnectorId();
	result.relation_name = relation.Name();
	result.explicit_inputs = std::move(explicit_inputs);
	result.projected_columns.reserve(relation.Columns().size());
	for (const auto &column : relation.Columns()) {
		result.projected_columns.push_back(column.Name());
	}
	result.requested_predicate = RequestedPredicate::Unrestricted();
	result.retained_predicate_scope = RetainedPredicateScope::UNRESTRICTED;
	result.orderings.clear();
	result.has_limit = false;
	result.has_offset = false;
	result.capabilities = {false, false, false, false, false, false, false, false, true, true};
	result.secret_reference = std::move(secret_reference);
	result.generation_identity = BuildPackageScanGenerationIdentity(identity);
	return result;
}

} // namespace cuac
