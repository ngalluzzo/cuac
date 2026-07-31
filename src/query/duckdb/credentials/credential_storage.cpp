#include "cuac/internal/query/credentials/credential_storage.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace duckdb {
namespace cuac_query_internal {
namespace {

static const std::size_t MAX_INDEX_BYTES = 128 * 1024;
static const std::size_t MAX_RECORD_BYTES = 16 * 1024;
static const std::size_t MAX_LIVE_RECORDS = 256;
static const char INDEX_FILE[] = "index";
static const char INDEX_PENDING_FILE[] = "index.pending";
static const char LOCK_FILE[] = "lock";
static const char PROJECT_DIRECTORY[] = "cuac";
static const char INDEX_MAGIC[] = "DAPIIDX1";
static const char RECORD_MAGIC[] = "DAPIREC1";

[[noreturn]] void ThrowStorageFailure() {
	throw InvalidInputException("[cuac][credential_provider] persistent credential storage operation failed");
}

[[noreturn]] void ThrowAutocommitRequired() {
	throw InvalidInputException("[cuac][credential_provider] persistent credential mutation requires autocommit");
}

[[noreturn]] void ThrowConflict() {
	throw InvalidInputException("[cuac][credential_provider] persistent credential already exists");
}

[[noreturn]] void ThrowMissing() {
	throw InvalidInputException("[cuac][credential_provider] persistent credential was not found");
}

void RequireAutoCommit(optional_ptr<CatalogTransaction> transaction) {
	if (!transaction || !transaction->context || !transaction->context->transaction.IsAutoCommit()) {
		ThrowAutocommitRequired();
	}
}

void AppendU16(std::vector<std::uint8_t> &output, std::uint16_t value) {
	output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffU));
	output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void AppendU32(std::vector<std::uint8_t> &output, std::uint32_t value) {
	for (int shift = 24; shift >= 0; shift -= 8) {
		output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
	}
}

void AppendU64(std::vector<std::uint8_t> &output, std::uint64_t value) {
	for (int shift = 56; shift >= 0; shift -= 8) {
		output.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
	}
}

std::uint64_t Checksum(const std::vector<std::uint8_t> &bytes, std::size_t length) noexcept {
	std::uint64_t result = 1469598103934665603ULL;
	for (std::size_t index = 0; index < length; index++) {
		result ^= bytes[index];
		result *= 1099511628211ULL;
	}
	return result;
}

void AppendString(std::vector<std::uint8_t> &output, const std::string &value) {
	if (value.empty() || value.size() > 65535) {
		ThrowStorageFailure();
	}
	AppendU16(output, static_cast<std::uint16_t>(value.size()));
	output.insert(output.end(), value.begin(), value.end());
}

class ByteReader {
public:
	ByteReader(const std::vector<std::uint8_t> &bytes_p, std::size_t limit_p)
	    : bytes(bytes_p), offset(0), limit(limit_p) {
	}

	std::uint8_t U8() {
		Require(1);
		return bytes[offset++];
	}

	std::uint16_t U16() {
		std::uint16_t result = 0;
		for (std::size_t index = 0; index < 2; index++) {
			result = static_cast<std::uint16_t>((result << 8) | U8());
		}
		return result;
	}

	std::uint32_t U32() {
		std::uint32_t result = 0;
		for (std::size_t index = 0; index < 4; index++) {
			result = (result << 8) | U8();
		}
		return result;
	}

	std::uint64_t U64() {
		std::uint64_t result = 0;
		for (std::size_t index = 0; index < 8; index++) {
			result = (result << 8) | U8();
		}
		return result;
	}

	std::string String() {
		const auto length = U16();
		if (length == 0) {
			ThrowStorageFailure();
		}
		Require(length);
		std::string result(reinterpret_cast<const char *>(&bytes[offset]), length);
		offset += length;
		return result;
	}

	CredentialIdentity Identity() {
		CredentialIdentity result {};
		Require(result.size());
		for (auto &byte : result) {
			byte = bytes[offset++];
		}
		if (!IsNonZeroIdentity(result)) {
			ThrowStorageFailure();
		}
		return result;
	}

	void Magic(const char *expected) {
		const std::size_t length = 8;
		Require(length);
		if (std::memcmp(&bytes[offset], expected, length) != 0) {
			ThrowStorageFailure();
		}
		offset += length;
	}

	bool AtEnd() const noexcept {
		return offset == limit;
	}

private:
	void Require(std::size_t count) {
		if (count > limit || offset > limit - count) {
			ThrowStorageFailure();
		}
	}

	const std::vector<std::uint8_t> &bytes;
	std::size_t offset;
	const std::size_t limit;
};

CredentialSource ParseSource(std::uint8_t value) {
	if (value == static_cast<std::uint8_t>(CredentialSource::CONFIG)) {
		return CredentialSource::CONFIG;
	}
	if (value == static_cast<std::uint8_t>(CredentialSource::ENVIRONMENT)) {
		return CredentialSource::ENVIRONMENT;
	}
	ThrowStorageFailure();
}

bool IsHexId(const std::string &value) noexcept {
	if (value.size() != 32) {
		return false;
	}
	for (const auto byte : value) {
		if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) {
			return false;
		}
	}
	return true;
}

std::string HexId(const CredentialIdentity &identity) {
	static const char DIGITS[] = "0123456789abcdef";
	std::string result;
	result.reserve(identity.size() * 2);
	for (const auto byte : identity) {
		result.push_back(DIGITS[(byte >> 4) & 0x0fU]);
		result.push_back(DIGITS[byte & 0x0fU]);
	}
	return result;
}

std::string NewRecordId() {
	return HexId(GenerateCredentialIdentity());
}

std::string RecordFile(const std::string &record_id) {
	if (!IsHexId(record_id)) {
		ThrowStorageFailure();
	}
	return "record-" + record_id + ".bin";
}

void CloseNoThrow(int &fd) noexcept {
	if (fd >= 0) {
		(void)close(fd);
		fd = -1;
	}
}

void CheckCancellation(cuac::ExecutionControl *control) {
	if (control && control->IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
}

void SyncFile(int fd) {
#ifdef F_FULLFSYNC
	if (fcntl(fd, F_FULLFSYNC) == 0) {
		return;
	}
#endif
	if (fsync(fd) != 0) {
		ThrowStorageFailure();
	}
}

void ValidateRegularPrivateFile(int fd) {
	struct stat status {};
	if (fstat(fd, &status) != 0 || !S_ISREG(status.st_mode) || status.st_uid != geteuid() ||
	    (status.st_mode & 0077) != 0 || status.st_nlink != 1) {
		ThrowStorageFailure();
	}
}

int OpenExistingFileAt(int directory_fd, const std::string &name, bool missing_ok) {
	// O_NONBLOCK is inert for regular files and prevents a hostile FIFO at any
	// fixed leaf name from turning descriptor validation into an unbounded wait.
	const auto fd = openat(directory_fd, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0) {
		if (missing_ok && errno == ENOENT) {
			return -1;
		}
		ThrowStorageFailure();
	}
	try {
		ValidateRegularPrivateFile(fd);
	} catch (...) {
		int owned = fd;
		CloseNoThrow(owned);
		throw;
	}
	return fd;
}

std::vector<std::uint8_t> ReadBoundedFileAt(int directory_fd, const std::string &name, std::size_t limit,
                                            bool missing_ok, bool &missing, cuac::ExecutionControl *control = nullptr) {
	missing = false;
	CheckCancellation(control);
	int fd = OpenExistingFileAt(directory_fd, name, missing_ok);
	if (fd < 0) {
		missing = true;
		return {};
	}
	try {
		CheckCancellation(control);
		struct stat status {};
		CheckCancellation(control);
		if (fstat(fd, &status) != 0 || status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) > limit) {
			ThrowStorageFailure();
		}
		CheckCancellation(control);
		std::vector<std::uint8_t> result(static_cast<std::size_t>(status.st_size));
		std::size_t offset = 0;
		while (offset < result.size()) {
			CheckCancellation(control);
			const auto count = read(fd, result.data() + offset, result.size() - offset);
			if (count <= 0) {
				ThrowStorageFailure();
			}
			offset += static_cast<std::size_t>(count);
			CheckCancellation(control);
		}
		CloseNoThrow(fd);
		return result;
	} catch (...) {
		CloseNoThrow(fd);
		throw;
	}
}

void WriteAll(int fd, const std::vector<std::uint8_t> &bytes) {
	std::size_t offset = 0;
	while (offset < bytes.size()) {
		const auto count = write(fd, bytes.data() + offset, bytes.size() - offset);
		if (count <= 0) {
			ThrowStorageFailure();
		}
		offset += static_cast<std::size_t>(count);
	}
}

std::vector<std::string> SplitAbsolutePath(const std::string &path) {
	if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos) {
		ThrowStorageFailure();
	}
	std::vector<std::string> result;
	std::size_t begin = 1;
	while (begin <= path.size()) {
		const auto end = path.find('/', begin);
		const auto component = path.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
		if (component == "." || component == "..") {
			ThrowStorageFailure();
		}
		if (!component.empty()) {
			result.push_back(component);
		}
		if (end == std::string::npos) {
			break;
		}
		begin = end + 1;
	}
	return result;
}

std::mutex &RegistryMutex() {
	static std::mutex mutex;
	return mutex;
}

std::map<DatabaseInstance *, weak_ptr<CredentialStorageState>> &Registry() {
	static std::map<DatabaseInstance *, weak_ptr<CredentialStorageState>> registry;
	return registry;
}

void RegisterState(DatabaseInstance &database, const shared_ptr<CredentialStorageState> &state) {
	std::lock_guard<std::mutex> guard(RegistryMutex());
	auto &slot = Registry()[&database];
	if (!slot.expired()) {
		ThrowStorageFailure();
	}
	slot = state;
}

void UnregisterState(DatabaseInstance &database, const shared_ptr<CredentialStorageState> &state) noexcept {
	try {
		std::lock_guard<std::mutex> guard(RegistryMutex());
		const auto found = Registry().find(&database);
		if (found != Registry().end() && found->second.lock() == state) {
			Registry().erase(found);
		}
	} catch (...) {
	}
}

struct PersistentMetadata {
	std::string name;
	CredentialSource source;
	CredentialIdentity authority;
	CredentialIdentity revision;
	std::string record_id;
};

using PersistentEntries = std::map<std::string, PersistentMetadata>;

std::string NormalizedName(const std::string &name) {
	if (name.empty()) {
		ThrowStorageFailure();
	}
	return StringUtil::Lower(name);
}

std::vector<std::uint8_t> EncodeIndex(const PersistentEntries &entries, const std::string &cleanup_id) {
	if (entries.size() > MAX_LIVE_RECORDS || (!cleanup_id.empty() && !IsHexId(cleanup_id))) {
		ThrowStorageFailure();
	}
	std::set<std::string> record_ids;
	std::vector<std::uint8_t> result;
	result.insert(result.end(), INDEX_MAGIC, INDEX_MAGIC + 8);
	AppendU32(result, static_cast<std::uint32_t>(entries.size()));
	result.push_back(cleanup_id.empty() ? 0 : 1);
	if (!cleanup_id.empty()) {
		AppendString(result, cleanup_id);
	}
	for (const auto &item : entries) {
		const auto &entry = item.second;
		if (!IsHexId(entry.record_id) || !record_ids.insert(entry.record_id).second ||
		    (!cleanup_id.empty() && entry.record_id == cleanup_id)) {
			ThrowStorageFailure();
		}
		AppendString(result, entry.name);
		result.push_back(static_cast<std::uint8_t>(entry.source));
		result.insert(result.end(), entry.authority.begin(), entry.authority.end());
		result.insert(result.end(), entry.revision.begin(), entry.revision.end());
		AppendString(result, entry.record_id);
	}
	AppendU64(result, Checksum(result, result.size()));
	if (result.size() > MAX_INDEX_BYTES) {
		ThrowStorageFailure();
	}
	return result;
}

void DecodeIndex(const std::vector<std::uint8_t> &bytes, PersistentEntries &entries, std::string &cleanup_id) {
	if (bytes.size() < 8 + 4 + 1 + 8) {
		ThrowStorageFailure();
	}
	const auto payload_size = bytes.size() - 8;
	ByteReader checksum_reader(bytes, bytes.size());
	for (std::size_t index = 0; index < payload_size; index++) {
		(void)checksum_reader.U8();
	}
	if (checksum_reader.U64() != Checksum(bytes, payload_size)) {
		ThrowStorageFailure();
	}
	ByteReader reader(bytes, payload_size);
	reader.Magic(INDEX_MAGIC);
	const auto count = reader.U32();
	if (count > MAX_LIVE_RECORDS) {
		ThrowStorageFailure();
	}
	const auto has_cleanup = reader.U8();
	if (has_cleanup > 1) {
		ThrowStorageFailure();
	}
	cleanup_id = has_cleanup ? reader.String() : std::string();
	if (!cleanup_id.empty() && !IsHexId(cleanup_id)) {
		ThrowStorageFailure();
	}
	PersistentEntries decoded;
	std::set<std::string> record_ids;
	for (std::uint32_t index = 0; index < count; index++) {
		PersistentMetadata entry;
		entry.name = reader.String();
		entry.source = ParseSource(reader.U8());
		entry.authority = reader.Identity();
		entry.revision = reader.Identity();
		entry.record_id = reader.String();
		if (!IsHexId(entry.record_id) || !record_ids.insert(entry.record_id).second ||
		    (!cleanup_id.empty() && entry.record_id == cleanup_id)) {
			ThrowStorageFailure();
		}
		const auto key = NormalizedName(entry.name);
		if (!decoded.emplace(key, entry).second) {
			ThrowStorageFailure();
		}
	}
	if (!reader.AtEnd()) {
		ThrowStorageFailure();
	}
	entries = std::move(decoded);
}

std::vector<std::uint8_t> EncodeRecord(const PersistentMetadata &metadata, const std::string &payload) {
	std::vector<std::uint8_t> result;
	result.insert(result.end(), RECORD_MAGIC, RECORD_MAGIC + 8);
	AppendString(result, metadata.record_id);
	AppendString(result, metadata.name);
	result.push_back(static_cast<std::uint8_t>(metadata.source));
	result.insert(result.end(), metadata.authority.begin(), metadata.authority.end());
	result.insert(result.end(), metadata.revision.begin(), metadata.revision.end());
	if (payload.empty() || payload.size() > 8192) {
		ThrowStorageFailure();
	}
	AppendU32(result, static_cast<std::uint32_t>(payload.size()));
	result.insert(result.end(), payload.begin(), payload.end());
	AppendU64(result, Checksum(result, result.size()));
	if (result.size() > MAX_RECORD_BYTES) {
		ThrowStorageFailure();
	}
	return result;
}

unique_ptr<CuacSecret> DecodeRecord(const std::vector<std::uint8_t> &bytes, const PersistentMetadata &expected) {
	if (bytes.size() < 8 + 2 + 2 + 1 + 16 + 16 + 4 + 8) {
		ThrowStorageFailure();
	}
	const auto payload_size = bytes.size() - 8;
	ByteReader checksum_reader(bytes, bytes.size());
	for (std::size_t index = 0; index < payload_size; index++) {
		(void)checksum_reader.U8();
	}
	if (checksum_reader.U64() != Checksum(bytes, payload_size)) {
		ThrowStorageFailure();
	}
	ByteReader reader(bytes, payload_size);
	reader.Magic(RECORD_MAGIC);
	const auto record_id = reader.String();
	const auto name = reader.String();
	const auto source = ParseSource(reader.U8());
	const auto authority = reader.Identity();
	const auto revision = reader.Identity();
	const auto value_size = reader.U32();
	if (value_size == 0 || value_size > 8192 || value_size > payload_size) {
		ThrowStorageFailure();
	}
	std::string payload;
	payload.reserve(value_size);
	for (std::uint32_t index = 0; index < value_size; index++) {
		payload.push_back(static_cast<char>(reader.U8()));
	}
	if (!reader.AtEnd() || record_id != expected.record_id || name != expected.name || source != expected.source ||
	    authority != expected.authority || revision != expected.revision) {
		ThrowStorageFailure();
	}
	return make_uniq<CuacSecret>(vector<string>(), source, name, std::move(payload), authority, revision);
}

unique_ptr<SecretEntry> MetadataEntry(const PersistentMetadata &metadata) {
	auto secret = make_uniq<CuacSecret>(vector<string>(), metadata.source, metadata.name, std::string(),
	                                    metadata.authority, metadata.revision);
	auto result = make_uniq<SecretEntry>(std::move(secret));
	result->persist_type = SecretPersistType::PERSISTENT;
	result->storage_mode = CuacPersistentStorage();
	return result;
}

} // namespace

class CredentialStorageState::Impl {
public:
	explicit Impl(DatabaseInstance &database_p)
	    : database(database_p), active(true), opened(false), directory_fd(-1), lock_fd(-1) {
	}

	~Impl() noexcept {
		CloseNoThrow(lock_fd);
		CloseNoThrow(directory_fd);
	}

	std::string CurrentRoot() {
		auto &manager = SecretManager::Get(database);
		if (!manager.PersistentSecretsEnabled()) {
			ThrowStorageFailure();
		}
		auto path = FileSystem::GetLocal(database).ExpandPath(manager.PersistentSecretPath());
		if (path.empty()) {
			ThrowStorageFailure();
		}
		if (path[0] != '/') {
			char working_directory[4096];
			if (!getcwd(working_directory, sizeof(working_directory))) {
				ThrowStorageFailure();
			}
			path = std::string(working_directory) + "/" + path;
		}
		(void)SplitAbsolutePath(path);
		return path;
	}

	bool PersistentSecretsEnabled() {
		RequireActive();
		return SecretManager::Get(database).PersistentSecretsEnabled();
	}

	void RequireActive() {
		if (!active) {
			ThrowStorageFailure();
		}
	}

	void MaybeFail(CredentialStorageTestFault expected) {
		if (test_fault != expected) {
			return;
		}
		if (test_fault_occurrence > 1) {
			test_fault_occurrence--;
			return;
		}
		test_fault = CredentialStorageTestFault::NONE;
		test_fault_occurrence = 0;
		ThrowStorageFailure();
	}

	void EnsureOpen(cuac::ExecutionControl *control = nullptr) {
		RequireActive();
		CheckCancellation(control);
		const auto current_root = CurrentRoot();
		CheckCancellation(control);
		if (opened) {
			if (current_root != root) {
				ThrowStorageFailure();
			}
			return;
		}

		int current = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (current < 0) {
			ThrowStorageFailure();
		}
		int project = -1;
		int lock = -1;
		try {
			CheckCancellation(control);
			const auto components = SplitAbsolutePath(current_root);
			for (const auto &component : components) {
				CheckCancellation(control);
				int next = openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				if (next < 0 && errno == ENOENT) {
					if (mkdirat(current, component.c_str(), 0700) != 0) {
						ThrowStorageFailure();
					}
					// Persist each newly linked path component before releasing its
					// parent descriptor. Later project-file fsyncs cannot make an
					// unsynchronized ancestor durable retroactively.
					SyncFile(current);
					next = openat(current, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
				}
				if (next < 0) {
					ThrowStorageFailure();
				}
				try {
					CheckCancellation(control);
				} catch (...) {
					CloseNoThrow(next);
					throw;
				}
				CloseNoThrow(current);
				current = next;
			}
			project = openat(current, PROJECT_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			if (project < 0 && errno == ENOENT) {
				if (mkdirat(current, PROJECT_DIRECTORY, 0700) != 0) {
					ThrowStorageFailure();
				}
				SyncFile(current);
				project = openat(current, PROJECT_DIRECTORY, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
			}
			CheckCancellation(control);
			if (project < 0) {
				ThrowStorageFailure();
			}
			struct stat project_status {};
			CheckCancellation(control);
			if (fstat(project, &project_status) != 0 || !S_ISDIR(project_status.st_mode) ||
			    project_status.st_uid != geteuid() || (project_status.st_mode & 0077) != 0) {
				CloseNoThrow(project);
				ThrowStorageFailure();
			}
			CheckCancellation(control);
			CloseNoThrow(current);
			current = -1;

			lock = openat(project, LOCK_FILE, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
			if (lock < 0 && errno == EEXIST) {
				lock = openat(project, LOCK_FILE, O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
			}
			CheckCancellation(control);
			if (lock < 0) {
				CloseNoThrow(project);
				ThrowStorageFailure();
			}
			try {
				ValidateRegularPrivateFile(lock);
				if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
					ThrowStorageFailure();
				}
				CheckCancellation(control);
			} catch (...) {
				CloseNoThrow(lock);
				CloseNoThrow(project);
				throw;
			}

			bool missing = false;
			auto index = ReadBoundedFileAt(project, INDEX_FILE, MAX_INDEX_BYTES, true, missing, control);
			PersistentEntries loaded_entries;
			std::string loaded_cleanup;
			if (!missing) {
				CheckCancellation(control);
				DecodeIndex(index, loaded_entries, loaded_cleanup);
				CheckCancellation(control);
			}
			root = current_root;
			entries = std::move(loaded_entries);
			cleanup_id = std::move(loaded_cleanup);
			directory_fd = project;
			lock_fd = lock;
			opened = true;
			project = -1;
			lock = -1;
		} catch (...) {
			CloseNoThrow(current);
			CloseNoThrow(lock);
			CloseNoThrow(project);
			throw;
		}
	}

	void RemoveRecordIfPresent(const std::string &record_id) {
		const auto filename = RecordFile(record_id);
		int fd = OpenExistingFileAt(directory_fd, filename, true);
		if (fd < 0) {
			return;
		}
		CloseNoThrow(fd);
		MaybeFail(CredentialStorageTestFault::REMOVE_RECORD);
		if (unlinkat(directory_fd, filename.c_str(), 0) != 0) {
			ThrowStorageFailure();
		}
		SyncFile(directory_fd);
	}

	void PublishIndex(PersistentEntries next_entries, std::string next_cleanup) {
		const auto bytes = EncodeIndex(next_entries, next_cleanup);
		int stale = OpenExistingFileAt(directory_fd, INDEX_PENDING_FILE, true);
		if (stale >= 0) {
			CloseNoThrow(stale);
			if (unlinkat(directory_fd, INDEX_PENDING_FILE, 0) != 0) {
				ThrowStorageFailure();
			}
		}
		int pending =
		    openat(directory_fd, INDEX_PENDING_FILE, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (pending < 0) {
			ThrowStorageFailure();
		}
		try {
			ValidateRegularPrivateFile(pending);
			WriteAll(pending, bytes);
			SyncFile(pending);
			CloseNoThrow(pending);
			MaybeFail(CredentialStorageTestFault::BEFORE_INDEX_RENAME);
			if (renameat(directory_fd, INDEX_PENDING_FILE, directory_fd, INDEX_FILE) != 0) {
				ThrowStorageFailure();
			}
			// renameat is the selection commit point. Swap the already-built state
			// without allocation before the fallible directory sync so this live
			// instance and a restart cannot disagree after a post-rename failure.
			entries.swap(next_entries);
			cleanup_id.swap(next_cleanup);
			MaybeFail(CredentialStorageTestFault::AFTER_INDEX_RENAME);
			SyncFile(directory_fd);
		} catch (...) {
			CloseNoThrow(pending);
			throw;
		}
	}

	void CleanPriorIntent() {
		if (cleanup_id.empty()) {
			return;
		}
		RemoveRecordIfPresent(cleanup_id);
		PublishIndex(entries, std::string());
	}

	void SettleCommittedCleanupNoThrow() noexcept {
		if (cleanup_id.empty()) {
			return;
		}
		try {
			RemoveRecordIfPresent(cleanup_id);
			PublishIndex(entries, std::string());
		} catch (...) {
			// The indexed mutation is already durable. Retain its cleanup intent
			// so a later successful mutation can settle the unreachable record;
			// cleanup failure must not report that the committed authority failed.
		}
	}

	void WriteRecord(const PersistentMetadata &metadata, const std::string &payload) {
		const auto bytes = EncodeRecord(metadata, payload);
		const auto filename = RecordFile(metadata.record_id);
		MaybeFail(CredentialStorageTestFault::WRITE_RECORD);
		int fd = openat(directory_fd, filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
		if (fd < 0) {
			ThrowStorageFailure();
		}
		try {
			ValidateRegularPrivateFile(fd);
			WriteAll(fd, bytes);
			SyncFile(fd);
			CloseNoThrow(fd);
			SyncFile(directory_fd);
		} catch (...) {
			CloseNoThrow(fd);
			throw;
		}
	}

	DatabaseInstance &database;
	std::mutex mutex;
	bool active;
	bool opened;
	std::string root;
	int directory_fd;
	int lock_fd;
	PersistentEntries entries;
	std::string cleanup_id;
	CredentialStorageTestFault test_fault = CredentialStorageTestFault::NONE;
	std::size_t test_fault_occurrence = 0;
};

CredentialStorageState::CredentialStorageState(DatabaseInstance &database)
    : impl(make_uniq<Impl>(database)), memory_may_contain_cuac(false) {
}

CredentialStorageState::~CredentialStorageState() noexcept {
	Shutdown();
}

void CredentialStorageState::MarkMemoryMayContainCuac() noexcept {
	memory_may_contain_cuac.store(true, std::memory_order_release);
}

bool CredentialStorageState::MemoryMayContainCuac() const noexcept {
	return memory_may_contain_cuac.load(std::memory_order_acquire);
}

unique_ptr<CuacSecret> CredentialStorageState::SelectPersistentCredential(const std::string &name, bool memory_selected,
                                                                          cuac::ExecutionControl &control) {
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	std::lock_guard<std::mutex> guard(impl->mutex);
	CheckCancellation(&control);
	if (!impl->PersistentSecretsEnabled()) {
		// DuckDB's persistent-secret switch does not revoke an admitted memory
		// credential. It excludes this storage without opening its path, while the
		// already-cloned memory entry remains the sole selected authority.
		return nullptr;
	}
	CheckCancellation(&control);
	impl->EnsureOpen(&control);
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	const auto found = impl->entries.find(NormalizedName(name));
	if (found == impl->entries.end()) {
		return nullptr;
	}
	if (memory_selected) {
		// Ambiguity is decided from index metadata before the provider reads a
		// persistent payload. Holding the storage lock from this decision through
		// the exact record read prevents persistent mutation from crossing the
		// selected scan snapshot.
		ThrowStorageFailure();
	}
	bool missing = false;
	auto bytes = ReadBoundedFileAt(impl->directory_fd, RecordFile(found->second.record_id), MAX_RECORD_BYTES, false,
	                               missing, &control);
	(void)missing;
	if (control.IsCancellationRequested()) {
		throw cuac::ExecutionCancelled();
	}
	return DecodeRecord(bytes, found->second);
}

unique_ptr<SecretEntry> CredentialStorageState::StorePersistent(unique_ptr<const BaseSecret> secret,
                                                                OnCreateConflict on_conflict,
                                                                optional_ptr<CatalogTransaction> transaction) {
	RequireAutoCommit(transaction);
	const auto *credential = dynamic_cast<const CuacSecret *>(secret.get());
	if (!credential || !secret->GetScope().empty() || !StringUtil::CIEquals(secret->GetType(), CuacSecretType())) {
		ThrowStorageFailure();
	}
	std::lock_guard<std::mutex> guard(impl->mutex);
	impl->EnsureOpen();
	const auto key = NormalizedName(secret->GetName());
	const auto found = impl->entries.find(key);
	const bool replacing = found != impl->entries.end();
	if (found != impl->entries.end()) {
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			ThrowConflict();
		}
		if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return MetadataEntry(found->second);
		}
		if (on_conflict != OnCreateConflict::REPLACE_ON_CONFLICT) {
			ThrowStorageFailure();
		}
	} else if (on_conflict == OnCreateConflict::ALTER_ON_CONFLICT) {
		ThrowStorageFailure();
	}
	if (found == impl->entries.end() && impl->entries.size() >= MAX_LIVE_RECORDS) {
		ThrowStorageFailure();
	}

	impl->CleanPriorIntent();
	PersistentMetadata metadata;
	metadata.name = secret->GetName();
	metadata.source = credential->Source();
	metadata.authority = replacing ? found->second.authority : credential->Authority();
	metadata.revision = credential->Revision();
	metadata.record_id = NewRecordId();
	const auto superseded = replacing ? found->second.record_id : std::string();

	impl->PublishIndex(impl->entries, metadata.record_id);
	impl->WriteRecord(metadata, credential->Payload());

	PersistentEntries next = impl->entries;
	next[key] = metadata;
	impl->PublishIndex(std::move(next), superseded);
	impl->SettleCommittedCleanupNoThrow();
	return MetadataEntry(metadata);
}

vector<SecretEntry> CredentialStorageState::AllPersistent(optional_ptr<CatalogTransaction>) {
	std::lock_guard<std::mutex> guard(impl->mutex);
	if (!impl->PersistentSecretsEnabled()) {
		return {};
	}
	impl->EnsureOpen();
	vector<SecretEntry> result;
	result.reserve(impl->entries.size());
	for (const auto &item : impl->entries) {
		result.push_back(*MetadataEntry(item.second));
	}
	return result;
}

void CredentialStorageState::DropPersistent(const string &name, OnEntryNotFound on_entry_not_found,
                                            optional_ptr<CatalogTransaction> transaction) {
	RequireAutoCommit(transaction);
	std::lock_guard<std::mutex> guard(impl->mutex);
	impl->EnsureOpen();
	const auto key = NormalizedName(name);
	const auto found = impl->entries.find(key);
	if (found == impl->entries.end()) {
		if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			ThrowMissing();
		}
		return;
	}
	impl->CleanPriorIntent();
	const auto removed_record = found->second.record_id;
	auto next = impl->entries;
	next.erase(key);
	impl->PublishIndex(std::move(next), removed_record);
	impl->SettleCommittedCleanupNoThrow();
}

unique_ptr<SecretEntry> CredentialStorageState::GetPersistentMetadata(const string &name,
                                                                      optional_ptr<CatalogTransaction>) {
	std::lock_guard<std::mutex> guard(impl->mutex);
	if (!impl->PersistentSecretsEnabled()) {
		return nullptr;
	}
	impl->EnsureOpen();
	const auto found = impl->entries.find(NormalizedName(name));
	return found == impl->entries.end() ? nullptr : MetadataEntry(found->second);
}

void CredentialStorageState::InstallTestFault(CredentialStorageTestFault fault, std::size_t occurrence) {
	std::lock_guard<std::mutex> guard(impl->mutex);
	if (fault == CredentialStorageTestFault::NONE || occurrence == 0 ||
	    impl->test_fault != CredentialStorageTestFault::NONE) {
		ThrowStorageFailure();
	}
	impl->test_fault = fault;
	impl->test_fault_occurrence = occurrence;
}

void CredentialStorageState::Shutdown() noexcept {
	try {
		std::lock_guard<std::mutex> guard(impl->mutex);
		if (!impl->active) {
			return;
		}
		impl->active = false;
		impl->opened = false;
		CloseNoThrow(impl->lock_fd);
		CloseNoThrow(impl->directory_fd);
		impl->entries.clear();
		impl->cleanup_id.clear();
	} catch (...) {
	}
}

shared_ptr<CredentialStorageState> LookupCredentialStorageState(DatabaseInstance &database) {
	std::lock_guard<std::mutex> guard(RegistryMutex());
	const auto found = Registry().find(&database);
	if (found == Registry().end()) {
		ThrowStorageFailure();
	}
	auto state = found->second.lock();
	if (!state) {
		ThrowStorageFailure();
	}
	return state;
}

class CuacSecretStorage final : public SecretStorage {
public:
	explicit CuacSecretStorage(DatabaseInstance &database_p)
	    : SecretStorage(CuacPersistentStorage(), 30), database(database_p),
	      state(make_shared_ptr<CredentialStorageState>(database_p)) {
		persistent = true;
		RegisterState(database, state);
	}

	~CuacSecretStorage() override {
		UnregisterState(database, state);
		state->Shutdown();
	}

	unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                    optional_ptr<CatalogTransaction> transaction) override {
		return state->StorePersistent(std::move(secret), on_conflict, transaction);
	}

	vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction> transaction) override {
		return state->AllPersistent(transaction);
	}

	void DropSecretByName(const string &name, OnEntryNotFound on_entry_not_found,
	                      optional_ptr<CatalogTransaction> transaction) override {
		state->DropPersistent(name, on_entry_not_found, transaction);
	}

	SecretMatch LookupSecret(const string &, const string &, optional_ptr<CatalogTransaction>) override {
		return SecretMatch();
	}

	unique_ptr<SecretEntry> GetSecretByName(const string &name, optional_ptr<CatalogTransaction> transaction) override {
		return state->GetPersistentMetadata(name, transaction);
	}

	bool IncludeInLookups() override {
		return false;
	}

private:
	DatabaseInstance &database;
	shared_ptr<CredentialStorageState> state;
};

unique_ptr<SecretStorage> CreateCuacSecretStorage(DatabaseInstance &database) {
	return make_uniq<CuacSecretStorage>(database);
}

} // namespace cuac_query_internal
} // namespace duckdb
