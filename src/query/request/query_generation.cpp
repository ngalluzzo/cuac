#include "cuac/query/query_generation.hpp"

#include <stdexcept>
#include <utility>

namespace cuac {

namespace {

bool IsSafeDiagnosticPart(const std::string &value, bool allow_empty) {
	if ((!allow_empty && value.empty()) || value.size() > 512) {
		return false;
	}
	for (const auto character : value) {
		const auto byte = static_cast<unsigned char>(character);
		if (byte < 0x20 || byte == 0x7f) {
			return false;
		}
	}
	return true;
}

} // namespace

QueryStagingError::QueryStagingError(std::string code_p, std::string phase_p, std::string file_p, std::uint64_t line_p,
                                     std::uint64_t column_p, std::string yaml_path_p, std::string safe_detail_p)
    : code(std::move(code_p)), phase(std::move(phase_p)), file(std::move(file_p)), line(line_p), column(column_p),
      yaml_path(std::move(yaml_path_p)), safe_detail(std::move(safe_detail_p)), message() {
	if (!IsSafeDiagnosticPart(code, false) || !IsSafeDiagnosticPart(phase, false) ||
	    !IsSafeDiagnosticPart(file, true) || !IsSafeDiagnosticPart(yaml_path, true) ||
	    !IsSafeDiagnosticPart(safe_detail, false) || (!file.empty() && file[0] == '/') ||
	    file.find("..") != std::string::npos || ((line == 0) != (column == 0)) || (line != 0 && file.empty()) ||
	    (!yaml_path.empty() && (file.empty() || yaml_path[0] != '$'))) {
		throw std::invalid_argument("Query staging diagnostic is not safely renderable");
	}
	message = code + ": " + safe_detail;
}

const char *QueryStagingError::what() const noexcept {
	return message.c_str();
}

const std::string &QueryStagingError::Code() const noexcept {
	return code;
}

const std::string &QueryStagingError::Phase() const noexcept {
	return phase;
}

const std::string &QueryStagingError::File() const noexcept {
	return file;
}

bool QueryStagingError::HasLineAndColumn() const noexcept {
	return line != 0;
}

std::uint64_t QueryStagingError::Line() const noexcept {
	return line;
}

std::uint64_t QueryStagingError::Column() const noexcept {
	return column;
}

const std::string &QueryStagingError::YamlPath() const noexcept {
	return yaml_path;
}

const std::string &QueryStagingError::SafeDetail() const noexcept {
	return safe_detail;
}

QueryScanPlanningService::~QueryScanPlanningService() noexcept = default;
QueryGenerationOwner::~QueryGenerationOwner() noexcept = default;
QueryPublicationLease::~QueryPublicationLease() noexcept = default;
QueryPackageStagingService::~QueryPackageStagingService() noexcept = default;

QueryPublishedGeneration::QueryPublishedGeneration(CompiledQueryRegistrationView registration_p,
                                                   std::shared_ptr<const QueryScanPlanningService> planning_p,
                                                   std::shared_ptr<const ScanExecutor> executor_p,
                                                   std::shared_ptr<const QueryGenerationOwner> owner_p)
    : registration(std::move(registration_p)), planning(std::move(planning_p)), executor(std::move(executor_p)),
      owner(std::move(owner_p)) {
	if (!registration.GenerationHandle().IsValid()) {
		throw std::invalid_argument("Query generation requires a valid Connector generation handle");
	}
	if (!planning) {
		throw std::invalid_argument("Query generation requires a planning service");
	}
	if (!executor) {
		throw std::invalid_argument("Query generation requires a scan executor");
	}
	if (!owner) {
		throw std::invalid_argument("Query generation requires an opaque Runtime owner");
	}
}

const CompiledQueryRegistrationView &QueryPublishedGeneration::Registration() const noexcept {
	return registration;
}

const std::shared_ptr<const QueryScanPlanningService> &QueryPublishedGeneration::Planning() const noexcept {
	return planning;
}

const std::shared_ptr<const ScanExecutor> &QueryPublishedGeneration::Executor() const noexcept {
	return executor;
}

const std::shared_ptr<const QueryGenerationOwner> &QueryPublishedGeneration::Owner() const noexcept {
	return owner;
}

QueryStagedGeneration::QueryStagedGeneration(std::shared_ptr<const QueryPublishedGeneration> generation_p,
                                             bool changed_p, std::unique_ptr<QueryPublicationLease> publication_lease_p)
    : generation(std::move(generation_p)), changed(changed_p), publication_lease(std::move(publication_lease_p)) {
	if (!generation) {
		throw std::invalid_argument("staged Query generation must not be empty");
	}
	if (changed != static_cast<bool>(publication_lease)) {
		throw std::invalid_argument("changed Query generation requires exactly one publication lease");
	}
}

const std::shared_ptr<const QueryPublishedGeneration> &QueryStagedGeneration::Generation() const noexcept {
	return generation;
}

bool QueryStagedGeneration::Changed() const noexcept {
	return changed;
}

const QueryPublicationLease *QueryStagedGeneration::PublicationLease() const noexcept {
	return publication_lease.get();
}

std::unique_ptr<QueryPublicationLease> QueryStagedGeneration::TakePublicationLease() noexcept {
	return std::move(publication_lease);
}

} // namespace cuac
