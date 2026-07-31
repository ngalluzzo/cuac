#include "cuac/internal/connector/fixtures/package_fixture_source.hpp"

#include "cuac/internal/connector/source/compiled_local_package.hpp"
#include "cuac/internal/connector/fixtures/package_fixture_limits.hpp"
#include "cuac/connector/content_digest.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <dirent.h>
#include <fcntl.h>
#include <set>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

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
		const auto result = value;
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

void CheckCancellation(PackageCancellation &cancellation) {
	if (cancellation.IsCancellationRequested()) {
		throw PackageCompilationCancelled();
	}
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

struct FixtureEntry {
	std::string name;
	FileIdentity identity;
};

struct DirectoryCapture {
	FileIdentity directory;
	std::vector<FixtureEntry> entries;
};

FileIdentity FstatIdentity(int fd, const std::string &file) {
	struct stat status;
	if (::fstat(fd, &status) != 0) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, file, "fixture identity could not be read");
	}
	return IdentityOf(status);
}

void ValidateLeaf(const struct stat &status, const std::string &file) {
	if (!S_ISREG(status.st_mode) || status.st_nlink != 1) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, file,
		                           "fixture leaves must be no-follow regular files with one link");
	}
}

bool IsBodyFileName(const std::string &name) {
	if (name.empty() || name.size() > 255 || name.front() < 'a' || name.front() > 'z') {
		return false;
	}
	for (const auto character : name) {
		if (!((character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') || character == '_' ||
		      character == '.' || character == '-')) {
			return false;
		}
	}
	return true;
}

DirectoryCapture CaptureDirectory(int fixture_fd, const PackageFixtureLimits &limits,
                                  PackageCancellation &cancellation) {
	DirectoryCapture result;
	result.directory = FstatIdentity(fixture_fd, "fixtures");
	FileDescriptor enumeration(::openat(fixture_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (enumeration.Get() < 0) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "fixture directory could not be enumerated");
	}
	DIR *raw = ::fdopendir(enumeration.Get());
	if (!raw) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "fixture directory could not be enumerated");
	}
	(void)enumeration.Release();
	DirectoryStream directory(raw);
	std::set<std::string> folded;
	std::uint64_t checkpoint = 0;
	errno = 0;
	while (const auto *entry = ::readdir(directory.Get())) {
		const std::string name = entry->d_name;
		if (name == "." || name == "..") {
			continue;
		}
		if (name != "index.yaml" && !IsBodyFileName(name)) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
			                           "fixture leaf name is outside the closed path grammar");
		}
		if (result.entries.size() >= limits.max_fixture_leaves) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::RESOURCE_EXHAUSTED, "fixtures",
			                           "fixture leaf budget is exhausted");
		}
		std::string folded_name(name);
		for (auto &character : folded_name) {
			if (character >= 'A' && character <= 'Z') {
				character = static_cast<char>(character - 'A' + 'a');
			}
		}
		if (!folded.insert(folded_name).second) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures/" + name,
			                           "fixture leaf names collide by ASCII case");
		}
		struct stat status;
		if (::fstatat(fixture_fd, name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures/" + name,
			                           "fixture leaf changed during enumeration");
		}
		ValidateLeaf(status, "fixtures/" + name);
		result.entries.push_back({name, IdentityOf(status)});
		if (++checkpoint == 64) {
			CheckCancellation(cancellation);
			checkpoint = 0;
		}
		errno = 0;
	}
	if (errno != 0 || !(result.directory == FstatIdentity(fixture_fd, "fixtures"))) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "fixture directory changed during enumeration");
	}
	CheckCancellation(cancellation);
	std::sort(result.entries.begin(), result.entries.end(),
	          [](const FixtureEntry &left, const FixtureEntry &right) { return left.name < right.name; });
	return result;
}

const FixtureEntry *FindEntry(const DirectoryCapture &capture, const std::string &name) {
	const auto found = std::lower_bound(
	    capture.entries.begin(), capture.entries.end(), name,
	    [](const FixtureEntry &entry, const std::string &candidate) { return entry.name < candidate; });
	return found != capture.entries.end() && found->name == name ? &*found : nullptr;
}

std::string ReadLeaf(int fixture_fd, const FixtureEntry &entry, std::uint64_t maximum,
                     PackageCancellation &cancellation) {
	CheckCancellation(cancellation);
	const std::string relative = "fixtures/" + entry.name;
	FileDescriptor file(::openat(fixture_fd, entry.name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
	if (file.Get() < 0) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, relative, "fixture leaf changed while opening");
	}
	struct stat status;
	if (::fstat(file.Get(), &status) != 0) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, relative,
		                           "fixture leaf identity could not be read");
	}
	ValidateLeaf(status, relative);
	const auto before = IdentityOf(status);
	if (!(before == entry.identity)) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, relative, "fixture leaf changed before reading");
	}
	if (before.size > maximum) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::RESOURCE_EXHAUSTED, relative,
		                           "fixture leaf byte budget is exhausted");
	}
	std::string bytes;
	bytes.reserve(static_cast<std::size_t>(before.size));
	char buffer[64 * 1024];
	while (true) {
		CheckCancellation(cancellation);
		const auto count = ::read(file.Get(), buffer, sizeof(buffer));
		if (count < 0) {
			if (errno == EINTR) {
				continue;
			}
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, relative, "fixture leaf could not be read");
		}
		if (count == 0) {
			break;
		}
		bytes.append(buffer, static_cast<std::size_t>(count));
	}
	if (bytes.size() != before.size || !(before == FstatIdentity(file.Get(), relative))) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, relative, "fixture leaf changed during reading");
	}
	return bytes;
}

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

} // namespace

FixtureSourceFailure::FixtureSourceFailure(FixtureSourceFailureKind kind_p, std::string file_p,
                                           std::string safe_message_p)
    : kind(kind_p), file(std::move(file_p)), safe_message(std::move(safe_message_p)) {
}

const char *FixtureSourceFailure::what() const noexcept {
	return safe_message.c_str();
}

FixtureSourceFailureKind FixtureSourceFailure::Kind() const noexcept {
	return kind;
}

const std::string &FixtureSourceFailure::File() const noexcept {
	return file;
}

class FixtureDirectoryCustody::Impl {
public:
	Impl(FileDescriptor root_p, FileDescriptor fixture_p, DirectoryCapture capture_p, std::string index_p,
	     PackageFixtureLimits limits_p)
	    : root(std::move(root_p)), fixture(std::move(fixture_p)), capture(std::move(capture_p)),
	      index(std::move(index_p)), limits(limits_p) {
	}

	FileDescriptor root;
	FileDescriptor fixture;
	DirectoryCapture capture;
	std::string index;
	PackageFixtureLimits limits;
};

FixtureDirectoryCustody::FixtureDirectoryCustody(std::unique_ptr<Impl> impl_p) : impl(std::move(impl_p)) {
}

FixtureDirectoryCustody FixtureDirectoryCustody::Open(const CompiledLocalPackage &package,
                                                      const PackageFixtureLimits &host_limits,
                                                      PackageCancellation &cancellation) {
	CheckCancellation(cancellation);
	const auto limits = EffectivePackageFixtureLimits(host_limits);
	FileDescriptor root(cuac::internal::CompiledLocalPackageAccess::DuplicateRootForFixtures(package));
	struct stat status;
	if (::fstatat(root.Get(), "fixtures", &status, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISDIR(status.st_mode)) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "required fixture directory is missing or invalid");
	}
	FileDescriptor fixture(::openat(root.Get(), "fixtures", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
	if (fixture.Get() < 0) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "required fixture directory changed while opening");
	}
	auto capture = CaptureDirectory(fixture.Get(), limits, cancellation);
	const auto *index = FindEntry(capture, "index.yaml");
	if (index == nullptr) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures/index.yaml",
		                           "required fixture index is missing");
	}
	auto index_bytes = ReadLeaf(fixture.Get(), *index, limits.max_index_bytes, cancellation);
	return FixtureDirectoryCustody(std::unique_ptr<Impl>(
	    new Impl(std::move(root), std::move(fixture), std::move(capture), std::move(index_bytes), limits)));
}

FixtureDirectoryCustody::FixtureDirectoryCustody(FixtureDirectoryCustody &&) noexcept = default;
FixtureDirectoryCustody &FixtureDirectoryCustody::operator=(FixtureDirectoryCustody &&) noexcept = default;
FixtureDirectoryCustody::~FixtureDirectoryCustody() noexcept = default;

const std::string &FixtureDirectoryCustody::IndexBytes() const noexcept {
	return impl->index;
}

std::map<std::string, std::string>
FixtureDirectoryCustody::ReadPayloads(const std::vector<std::pair<std::string, std::string>> &referenced_payloads,
                                      PackageCancellation &cancellation) {
	std::set<std::string> unique;
	for (const auto &reference : referenced_payloads) {
		if (reference.first == "index.yaml" || !unique.insert(reference.first).second ||
		    FindEntry(impl->capture, reference.first) == nullptr) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures/" + reference.first,
			                           "fixture payload references are stale, repeated, or missing");
		}
	}
	if (unique.size() + 1 != impl->capture.entries.size()) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "fixture payload file set differs from the index");
	}
	std::map<std::string, std::string> payloads;
	std::uint64_t aggregate = 0;
	for (const auto &reference : referenced_payloads) {
		const auto *entry = FindEntry(impl->capture, reference.first);
		auto bytes = ReadLeaf(impl->fixture.Get(), *entry, impl->limits.max_payload_bytes, cancellation);
		if (bytes.size() > impl->limits.max_aggregate_payload_bytes - aggregate) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::RESOURCE_EXHAUSTED, "fixtures/" + reference.first,
			                           "aggregate fixture payload budget is exhausted");
		}
		aggregate += bytes.size();
		if ("sha256." + ComputeSha256Hex(bytes) != reference.second) {
			throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures/" + reference.first,
			                           "fixture payload digest differs from the index");
		}
		payloads.emplace(reference.first, std::move(bytes));
	}
	const auto after = CaptureDirectory(impl->fixture.Get(), impl->limits, cancellation);
	if (!SameCapture(impl->capture, after)) {
		throw FixtureSourceFailure(FixtureSourceFailureKind::MISMATCH, "fixtures",
		                           "fixture directory changed during acquisition");
	}
	return payloads;
}

} // namespace internal
} // namespace connector
} // namespace cuac
