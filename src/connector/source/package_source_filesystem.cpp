#include "cuac/internal/connector/source/package_source_internal.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cuac {
namespace connector {
namespace internal {
namespace {

class FileDescriptor {
public:
	explicit FileDescriptor(int value_p = -1) noexcept : value(value_p) {
	}
	~FileDescriptor() noexcept {
		if (value >= 0) {
			::close(value);
		}
	}
	FileDescriptor(FileDescriptor &&other) noexcept : value(other.Release()) {
	}
	FileDescriptor &operator=(FileDescriptor &&other) noexcept {
		if (this != &other) {
			if (value >= 0) {
				::close(value);
			}
			value = other.Release();
		}
		return *this;
	}
	FileDescriptor(const FileDescriptor &) = delete;
	FileDescriptor &operator=(const FileDescriptor &) = delete;

	int Get() const noexcept {
		return value;
	}
	int Release() noexcept {
		const int result = value;
		value = -1;
		return result;
	}

private:
	int value;
};

class DirectoryStream {
public:
	explicit DirectoryStream(DIR *value_p) noexcept : value(value_p) {
	}
	~DirectoryStream() noexcept {
		if (value) {
			::closedir(value);
		}
	}
	DirectoryStream(const DirectoryStream &) = delete;
	DirectoryStream &operator=(const DirectoryStream &) = delete;

	DIR *Get() const noexcept {
		return value;
	}

private:
	DIR *value;
};

PackageSourceLimits EffectiveLimits(const PackageSourceLimits &host) {
	if (host.max_semantic_files == 0 || host.max_root_entries == 0 || host.max_relation_entries == 0 ||
	    host.max_aggregate_entries == 0 || host.max_file_bytes == 0 || host.max_aggregate_bytes == 0 ||
	    host.max_relative_path_bytes == 0) {
		throw std::invalid_argument("package source limits must be positive");
	}
	const auto spec = PackageSourceLimits::V1();
	return {std::min(host.max_semantic_files, spec.max_semantic_files),
	        std::min(host.max_root_entries, spec.max_root_entries),
	        std::min(host.max_relation_entries, spec.max_relation_entries),
	        std::min(host.max_aggregate_entries, spec.max_aggregate_entries),
	        std::min(host.max_file_bytes, spec.max_file_bytes),
	        std::min(host.max_aggregate_bytes, spec.max_aggregate_bytes),
	        std::min(host.max_relative_path_bytes, spec.max_relative_path_bytes)};
}

void CheckCancellation(PackageCancellation &cancellation, const std::string &path = std::string()) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageSourceError(PackageSourceErrorCode::CANCELLED, path, "package source acquisition was cancelled");
	}
}

bool IsContinuation(unsigned char value) {
	return (value & 0xc0U) == 0x80U;
}

bool IsValidUtf8(const std::string &value) {
	for (std::size_t index = 0; index < value.size();) {
		const auto lead = static_cast<unsigned char>(value[index]);
		if (lead <= 0x7fU) {
			index++;
			continue;
		}
		if (lead >= 0xc2U && lead <= 0xdfU) {
			if (index + 1 >= value.size() || !IsContinuation(static_cast<unsigned char>(value[index + 1]))) {
				return false;
			}
			index += 2;
			continue;
		}
		if (lead >= 0xe0U && lead <= 0xefU) {
			if (index + 2 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			if (!IsContinuation(second) || !IsContinuation(static_cast<unsigned char>(value[index + 2])) ||
			    (lead == 0xe0U && second < 0xa0U) || (lead == 0xedU && second >= 0xa0U)) {
				return false;
			}
			index += 3;
			continue;
		}
		if (lead >= 0xf0U && lead <= 0xf4U) {
			if (index + 3 >= value.size()) {
				return false;
			}
			const auto second = static_cast<unsigned char>(value[index + 1]);
			if (!IsContinuation(second) || !IsContinuation(static_cast<unsigned char>(value[index + 2])) ||
			    !IsContinuation(static_cast<unsigned char>(value[index + 3])) || (lead == 0xf0U && second < 0x90U) ||
			    (lead == 0xf4U && second >= 0x90U)) {
				return false;
			}
			index += 4;
			continue;
		}
		return false;
	}
	return true;
}

std::string AsciiFold(const std::string &value) {
	std::string folded(value);
	for (auto &character : folded) {
		if (character >= 'A' && character <= 'Z') {
			character = static_cast<char>(character - 'A' + 'a');
		}
	}
	return folded;
}

struct FileIdentity {
	std::uint64_t device;
	std::uint64_t inode;
	std::uint64_t mode;
	std::uint64_t links;
	std::uint64_t size;
	std::int64_t modified_seconds;
	std::int64_t modified_nanoseconds;
	std::int64_t changed_seconds;
	std::int64_t changed_nanoseconds;
};

FileIdentity IdentityOf(const struct stat &status) {
#if defined(__APPLE__)
	return {static_cast<std::uint64_t>(status.st_dev),
	        static_cast<std::uint64_t>(status.st_ino),
	        static_cast<std::uint64_t>(status.st_mode),
	        static_cast<std::uint64_t>(status.st_nlink),
	        status.st_size < 0 ? 0 : static_cast<std::uint64_t>(status.st_size),
	        static_cast<std::int64_t>(status.st_mtimespec.tv_sec),
	        static_cast<std::int64_t>(status.st_mtimespec.tv_nsec),
	        static_cast<std::int64_t>(status.st_ctimespec.tv_sec),
	        static_cast<std::int64_t>(status.st_ctimespec.tv_nsec)};
#else
	return {static_cast<std::uint64_t>(status.st_dev),
	        static_cast<std::uint64_t>(status.st_ino),
	        static_cast<std::uint64_t>(status.st_mode),
	        static_cast<std::uint64_t>(status.st_nlink),
	        status.st_size < 0 ? 0 : static_cast<std::uint64_t>(status.st_size),
	        static_cast<std::int64_t>(status.st_mtim.tv_sec),
	        static_cast<std::int64_t>(status.st_mtim.tv_nsec),
	        static_cast<std::int64_t>(status.st_ctim.tv_sec),
	        static_cast<std::int64_t>(status.st_ctim.tv_nsec)};
#endif
}

bool operator==(const FileIdentity &left, const FileIdentity &right) {
	return left.device == right.device && left.inode == right.inode && left.mode == right.mode &&
	       left.links == right.links && left.size == right.size && left.modified_seconds == right.modified_seconds &&
	       left.modified_nanoseconds == right.modified_nanoseconds && left.changed_seconds == right.changed_seconds &&
	       left.changed_nanoseconds == right.changed_nanoseconds;
}

struct DirectoryEntryIdentity {
	std::string name;
	FileIdentity identity;
};

struct DirectoryCapture {
	FileIdentity directory;
	std::vector<DirectoryEntryIdentity> entries;
};

bool SameCapture(const DirectoryCapture &left, const DirectoryCapture &right) {
	if (!(left.directory == right.directory) || left.entries.size() != right.entries.size()) {
		return false;
	}
	for (std::size_t index = 0; index < left.entries.size(); index++) {
		if (left.entries[index].name != right.entries[index].name ||
		    !(left.entries[index].identity == right.entries[index].identity)) {
			return false;
		}
	}
	return true;
}

const DirectoryEntryIdentity *FindEntry(const DirectoryCapture &capture, const std::string &name) {
	const auto found = std::lower_bound(
	    capture.entries.begin(), capture.entries.end(), name,
	    [](const DirectoryEntryIdentity &entry, const std::string &candidate) { return entry.name < candidate; });
	return found != capture.entries.end() && found->name == name ? &*found : nullptr;
}

FileIdentity FstatIdentity(int fd, const std::string &path) {
	struct stat status;
	if (::fstat(fd, &status) != 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, path,
		                         "package source identity could not be read");
	}
	return IdentityOf(status);
}

void ValidateEntryType(const struct stat &status, const std::string &path) {
	if (S_ISLNK(status.st_mode)) {
		throw PackageSourceError(PackageSourceErrorCode::SYMBOLIC_LINK, path,
		                         "symbolic links are forbidden in package custody");
	}
	if (S_ISREG(status.st_mode)) {
		if (status.st_nlink != 1) {
			throw PackageSourceError(PackageSourceErrorCode::HARD_LINK, path,
			                         "hard-linked package leaves are forbidden");
		}
		return;
	}
	if (!S_ISDIR(status.st_mode)) {
		throw PackageSourceError(PackageSourceErrorCode::ENTRY_TYPE, path,
		                         "special package filesystem entries are forbidden");
	}
}

void AdmitDirectoryEntryName(const std::string &name, const std::string &prefix, std::uint64_t entry_limit,
                             std::size_t admitted_entries, std::uint64_t aggregate_entries,
                             const PackageSourceLimits &limits, std::set<std::string> &folded_names) {
	const std::string relative = prefix.empty() ? name : prefix + "/" + name;
	if (name.empty() || name.find('/') != std::string::npos || name.find('\\') != std::string::npos ||
	    !IsValidUtf8(name) || relative.size() > limits.max_relative_path_bytes) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, relative, "package entry path is not admitted");
	}
	if (admitted_entries >= entry_limit || aggregate_entries >= limits.max_aggregate_entries) {
		throw PackageSourceError(PackageSourceErrorCode::RESOURCE_EXHAUSTED, prefix,
		                         "package directory-entry budget is exhausted");
	}
	if (!folded_names.insert(AsciiFold(name)).second) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, relative,
		                         "package entry names collide by ASCII case");
	}
}

DirectoryCapture CaptureDirectory(int fd, const std::string &prefix, std::uint64_t entry_limit,
                                  std::uint64_t &aggregate_entries, const PackageSourceLimits &limits,
                                  PackageCancellation &cancellation, PackageCompilationPhaseHook *phase_hook) {
	DirectoryCapture capture;
	capture.directory = FstatIdentity(fd, prefix);
	// dup(2) shares a directory stream offset. Opening "." creates an
	// independent description so both identity captures start at entry zero.
	FileDescriptor enumeration(::openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (enumeration.Get() < 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, prefix,
		                         "package directory could not be enumerated");
	}
	DIR *opened_directory = ::fdopendir(enumeration.Get());
	if (!opened_directory) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, prefix,
		                         "package directory could not be enumerated");
	}
	(void)enumeration.Release();
	DirectoryStream directory(opened_directory);
	if (phase_hook != nullptr) {
		phase_hook->BeforeCancellationCheck(PackageCompilationCheckpoint::SOURCE_ENUMERATION);
	}
	CheckCancellation(cancellation, prefix);
	std::set<std::string> folded_names;
	std::uint64_t since_checkpoint = 0;
	errno = 0;
	while (const auto *entry = ::readdir(directory.Get())) {
		const std::string name(entry->d_name);
		if (name == "." || name == "..") {
			continue;
		}
		const std::string relative = prefix.empty() ? name : prefix + "/" + name;
		AdmitDirectoryEntryName(name, prefix, entry_limit, capture.entries.size(), aggregate_entries, limits,
		                        folded_names);
		struct stat status;
		if (::fstatat(fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
			throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative,
			                         "package entry changed during enumeration");
		}
		ValidateEntryType(status, relative);
		capture.entries.push_back({name, IdentityOf(status)});
		aggregate_entries++;
		if (++since_checkpoint == 64) {
			CheckCancellation(cancellation, prefix);
			since_checkpoint = 0;
		}
		errno = 0;
	}
	const int read_error = errno;
	if (read_error != 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, prefix,
		                         "package directory changed during enumeration");
	}
	CheckCancellation(cancellation, prefix);
	if (!(capture.directory == FstatIdentity(fd, prefix))) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, prefix,
		                         "package directory changed during enumeration");
	}
	std::sort(
	    capture.entries.begin(), capture.entries.end(),
	    [](const DirectoryEntryIdentity &left, const DirectoryEntryIdentity &right) { return left.name < right.name; });
	return capture;
}

FileDescriptor OpenAbsoluteRoot(const std::string &absolute_root, PackageCancellation &cancellation) {
	if (absolute_root.empty() || absolute_root[0] != '/' || absolute_root.find('\0') != std::string::npos ||
	    !IsValidUtf8(absolute_root)) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
		                         "package root must be an absolute UTF-8 path");
	}
	FileDescriptor current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
	if (current.Get() < 0) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "", "package root could not be opened");
	}
	std::size_t begin = 1;
	while (begin < absolute_root.size()) {
		const auto slash = absolute_root.find('/', begin);
		const auto end = slash == std::string::npos ? absolute_root.size() : slash;
		const auto component = absolute_root.substr(begin, end - begin);
		if (component.empty() || component == "." || component == ".." || component.find('\\') != std::string::npos) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
			                         "package root components must be normalized");
		}
		CheckCancellation(cancellation);
		struct stat status;
		if (::fstatat(current.Get(), component.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
			                         "package root component could not be opened");
		}
		if (S_ISLNK(status.st_mode)) {
			throw PackageSourceError(PackageSourceErrorCode::SYMBOLIC_LINK, "",
			                         "symbolic links are forbidden in the package root");
		}
		if (!S_ISDIR(status.st_mode)) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
			                         "package root component is not a directory");
		}
		FileDescriptor next(
		    ::openat(current.Get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
		if (next.Get() < 0) {
			throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, "",
			                         "package root component changed while opening");
		}
		if (!(IdentityOf(status) == FstatIdentity(next.Get(), ""))) {
			throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, "",
			                         "package root component changed while opening");
		}
		current = std::move(next);
		if (slash == std::string::npos) {
			break;
		}
		begin = slash + 1;
		if (begin == absolute_root.size()) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
			                         "package root components must be normalized");
		}
	}
	return current;
}

FileDescriptor DuplicateRoot(int root_fd) {
#if defined(F_DUPFD_CLOEXEC)
	const int duplicated = ::fcntl(root_fd, F_DUPFD_CLOEXEC, 0);
#else
	const int duplicated = ::dup(root_fd);
	if (duplicated >= 0) {
		::fcntl(duplicated, F_SETFD, FD_CLOEXEC);
	}
#endif
	if (duplicated < 0) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_ROOT, "",
		                         "retained package root is no longer available");
	}
	return FileDescriptor(duplicated);
}

FileDescriptor OpenRelations(int root_fd) {
	struct stat status;
	if (::fstatat(root_fd, "relations", &status, AT_SYMLINK_NOFOLLOW) != 0) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, "relations",
		                         "required package directory is missing");
	}
	if (S_ISLNK(status.st_mode)) {
		throw PackageSourceError(PackageSourceErrorCode::SYMBOLIC_LINK, "relations",
		                         "symbolic links are forbidden in package custody");
	}
	if (!S_ISDIR(status.st_mode)) {
		throw PackageSourceError(PackageSourceErrorCode::ENTRY_TYPE, "relations",
		                         "required package entry is not a directory");
	}
	FileDescriptor result(::openat(root_fd, "relations", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (result.Get() < 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, "relations",
		                         "required package directory changed while opening");
	}
	return result;
}

std::string ReadLeaf(int directory_fd, const std::string &name, const std::string &relative_path,
                     const FileIdentity &captured, const PackageSourceLimits &limits, std::uint64_t &aggregate_bytes,
                     PackageCancellation &cancellation) {
	CheckCancellation(cancellation, relative_path);
	FileDescriptor file(::openat(directory_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (file.Get() < 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative_path,
		                         "package source changed while opening");
	}
	struct stat status;
	if (::fstat(file.Get(), &status) != 0) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative_path,
		                         "package source identity could not be read");
	}
	ValidateEntryType(status, relative_path);
	const auto before = IdentityOf(status);
	if (!(before == captured)) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative_path,
		                         "package source changed before reading");
	}
	if (before.size > limits.max_file_bytes || before.size > limits.max_aggregate_bytes - aggregate_bytes) {
		throw PackageSourceError(PackageSourceErrorCode::RESOURCE_EXHAUSTED, relative_path,
		                         "package source byte budget is exhausted");
	}
	std::string bytes;
	bytes.reserve(static_cast<std::size_t>(before.size));
	char buffer[64 * 1024];
	while (true) {
		CheckCancellation(cancellation, relative_path);
		const auto count = ::read(file.Get(), buffer, sizeof(buffer));
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative_path,
			                         "package source could not be read completely");
		}
		if (count == 0) {
			break;
		}
		const auto amount = static_cast<std::uint64_t>(count);
		if (amount > limits.max_file_bytes - bytes.size() ||
		    amount > limits.max_aggregate_bytes - aggregate_bytes - bytes.size()) {
			throw PackageSourceError(PackageSourceErrorCode::RESOURCE_EXHAUSTED, relative_path,
			                         "package source byte budget is exhausted");
		}
		bytes.append(buffer, static_cast<std::size_t>(count));
	}
	CheckCancellation(cancellation, relative_path);
	if (::fstat(file.Get(), &status) != 0 || !(IdentityOf(status) == before) || bytes.size() != before.size) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, relative_path,
		                         "package source changed while reading");
	}
	aggregate_bytes += bytes.size();
	return bytes;
}

bool EndsWithYamlCaseInsensitive(const std::string &name) {
	return name.size() >= 5 && AsciiFold(name.substr(name.size() - 5)) == ".yaml";
}

} // namespace

void ValidatePackageDirectoryEntryNameCapture(const std::vector<std::string> &names, const std::string &prefix,
                                              std::uint64_t entry_limit, const PackageSourceLimits &host_limits,
                                              PackageCancellation &cancellation) {
	const auto limits = EffectiveLimits(host_limits);
	std::set<std::string> folded_names;
	std::uint64_t aggregate_entries = 0;
	for (const auto &name : names) {
		CheckCancellation(cancellation, prefix);
		AdmitDirectoryEntryName(name, prefix, entry_limit, static_cast<std::size_t>(aggregate_entries),
		                        aggregate_entries, limits, folded_names);
		aggregate_entries++;
	}
	CheckCancellation(cancellation, prefix);
}

class PackageDirectoryCustody::Impl {
public:
	Impl(FileDescriptor root_p, PackageSourceLimits limits_p, PackageCancellation &cancellation,
	     PackageCompilationPhaseHook *phase_hook)
	    : limits(limits_p), root(std::move(root_p)), relations(OpenRelations(root.Get())), aggregate_bytes(0),
	      verified(false) {
		std::uint64_t aggregate_entries = 0;
		root_before = CaptureDirectory(root.Get(), "", limits.max_root_entries, aggregate_entries, limits, cancellation,
		                               phase_hook);
		relations_before = CaptureDirectory(relations.Get(), "relations", limits.max_relation_entries,
		                                    aggregate_entries, limits, cancellation, phase_hook);
		const auto *manifest = FindEntry(root_before, "connector.yaml");
		const auto *relation_directory = FindEntry(root_before, "relations");
		if (!manifest || !S_ISREG(static_cast<mode_t>(manifest->identity.mode)) || !relation_directory ||
		    !S_ISDIR(static_cast<mode_t>(relation_directory->identity.mode)) ||
		    !(relation_directory->identity == FstatIdentity(relations.Get(), "relations"))) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, "connector.yaml",
			                         "package root is missing its manifest or relations directory");
		}
	}

	PackageSourceLimits limits;
	FileDescriptor root;
	FileDescriptor relations;
	DirectoryCapture root_before;
	DirectoryCapture relations_before;
	std::uint64_t aggregate_bytes;
	bool verified;
};

PackageDirectoryCustody::PackageDirectoryCustody(std::unique_ptr<Impl> impl_p) : impl(std::move(impl_p)) {
}

PackageDirectoryCustody PackageDirectoryCustody::OpenAbsolute(const std::string &absolute_root,
                                                              const PackageSourceLimits &host_limits,
                                                              PackageCancellation &cancellation,
                                                              PackageCompilationPhaseHook *phase_hook) {
	const auto limits = EffectiveLimits(host_limits);
	return PackageDirectoryCustody(std::unique_ptr<Impl>(
	    new Impl(OpenAbsoluteRoot(absolute_root, cancellation), limits, cancellation, phase_hook)));
}

PackageDirectoryCustody PackageDirectoryCustody::OpenRetained(int retained_root_fd,
                                                              const PackageSourceLimits &host_limits,
                                                              PackageCancellation &cancellation) {
	const auto limits = EffectiveLimits(host_limits);
	return PackageDirectoryCustody(
	    std::unique_ptr<Impl>(new Impl(DuplicateRoot(retained_root_fd), limits, cancellation, nullptr)));
}

PackageDirectoryCustody::PackageDirectoryCustody(PackageDirectoryCustody &&other) noexcept
    : impl(std::move(other.impl)) {
}

PackageDirectoryCustody &PackageDirectoryCustody::operator=(PackageDirectoryCustody &&other) noexcept {
	impl = std::move(other.impl);
	return *this;
}

PackageDirectoryCustody::~PackageDirectoryCustody() noexcept {
}

const PackageSourceLimits &PackageDirectoryCustody::Limits() const noexcept {
	return impl->limits;
}

std::string PackageDirectoryCustody::ReadManifest(PackageCancellation &cancellation) {
	const auto *entry = FindEntry(impl->root_before, "connector.yaml");
	return ReadLeaf(impl->root.Get(), "connector.yaml", "connector.yaml", entry->identity, impl->limits,
	                impl->aggregate_bytes, cancellation);
}

void PackageDirectoryCustody::ValidateListedRelations(const std::vector<std::string> &relation_ids) const {
	std::set<std::string> expected;
	for (const auto &id : relation_ids) {
		expected.insert(id + ".yaml");
	}
	for (const auto &entry : impl->relations_before.entries) {
		if (EndsWithYamlCaseInsensitive(entry.name) && expected.count(entry.name) == 0) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, "relations/" + entry.name,
			                         "unlisted relation YAML is forbidden");
		}
	}
	for (const auto &name : expected) {
		const auto *entry = FindEntry(impl->relations_before, name);
		if (!entry || !S_ISREG(static_cast<mode_t>(entry->identity.mode))) {
			throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, "relations/" + name,
			                         "listed relation source is missing or not a regular file");
		}
	}
}

std::string PackageDirectoryCustody::ReadRelation(const std::string &relation_id, PackageCancellation &cancellation) {
	const auto name = relation_id + ".yaml";
	const auto relative = "relations/" + name;
	const auto *entry = FindEntry(impl->relations_before, name);
	if (!entry) {
		throw PackageSourceError(PackageSourceErrorCode::INVALID_PATH, relative, "listed relation source is missing");
	}
	return ReadLeaf(impl->relations.Get(), name, relative, entry->identity, impl->limits, impl->aggregate_bytes,
	                cancellation);
}

void PackageDirectoryCustody::VerifyUnchanged(PackageCancellation &cancellation) {
	if (impl->verified) {
		return;
	}
	std::uint64_t aggregate_entries = 0;
	const auto root_after = CaptureDirectory(impl->root.Get(), "", impl->limits.max_root_entries, aggregate_entries,
	                                         impl->limits, cancellation, nullptr);
	const auto relations_after = CaptureDirectory(impl->relations.Get(), "relations", impl->limits.max_relation_entries,
	                                              aggregate_entries, impl->limits, cancellation, nullptr);
	if (!SameCapture(impl->root_before, root_after) || !SameCapture(impl->relations_before, relations_after)) {
		throw PackageSourceError(PackageSourceErrorCode::IDENTITY_CHANGED, "",
		                         "package source entry set or identity changed during acquisition");
	}
	CheckCancellation(cancellation);
	impl->verified = true;
}

int PackageDirectoryCustody::ReleaseRoot() noexcept {
	return impl ? impl->root.Release() : -1;
}

} // namespace internal
} // namespace connector
} // namespace cuac
