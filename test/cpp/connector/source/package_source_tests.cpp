#include "cuac/internal/connector/source/package_source.hpp"

#include "connector/compiler/test_support.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <ftw.h>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using cuac::connector::AcquirePackageSource;
using cuac::connector::PackageCancellation;
using cuac::connector::PackageSourceError;
using cuac::connector::PackageSourceErrorCode;
using cuac::connector::PackageSourceLimits;
using cuac::connector::PackageSourceSnapshot;
using cuac::connector::ReacquirePackageSource;
using cuac_test::NeverCancel;
using cuac_test::ReadFile;
using cuac_test::Require;

int RemoveEntry(const char *path, const struct stat *, int, struct FTW *) {
	return ::remove(path);
}

class TempTree {
public:
	TempTree() {
		char pattern[] = "/tmp/cuac-package-XXXXXX";
		char *created = ::mkdtemp(pattern);
		if (!created) {
			throw std::runtime_error("could not create temporary package tree");
		}
		path = created;
	}
	~TempTree() noexcept {
		(void)::nftw(path.c_str(), RemoveEntry, 32, FTW_DEPTH | FTW_PHYS);
	}
	const std::string &Path() const noexcept {
		return path;
	}

private:
	std::string path;
};

void MakeDirectory(const std::string &path) {
	if (::mkdir(path.c_str(), 0700) != 0) {
		throw std::runtime_error("could not create test directory");
	}
}

void WriteFile(const std::string &path, const std::string &bytes, int flags = O_WRONLY | O_CREAT | O_TRUNC) {
	const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
	if (fd < 0) {
		throw std::runtime_error("could not create test file");
	}
	std::size_t written = 0;
	while (written < bytes.size()) {
		const auto count = ::write(fd, bytes.data() + written, bytes.size() - written);
		if (count < 0) {
			const int saved = errno;
			::close(fd);
			errno = saved;
			throw std::runtime_error("could not write test file");
		}
		written += static_cast<std::size_t>(count);
	}
	if (::close(fd) != 0) {
		throw std::runtime_error("could not close test file");
	}
}

std::string CreateGithubPackage(const std::string &parent, const std::string &name) {
	const auto root = parent + "/" + name;
	MakeDirectory(root);
	MakeDirectory(root + "/relations");
	WriteFile(root + "/connector.yaml", ReadFile("connectors/github/connector.yaml"));
	const std::vector<std::string> relations = {"authenticated_repositories", "authenticated_user",
	                                            "duckdb_login_search_page", "viewer_repository_metrics"};
	for (const auto &relation : relations) {
		WriteFile(root + "/relations/" + relation + ".yaml",
		          ReadFile("connectors/github/relations/" + relation + ".yaml"));
	}
	return root;
}

template <class Function>
void RequireSourceFailure(PackageSourceErrorCode expected, Function function) {
	try {
		function();
		throw std::runtime_error("invalid package source was accepted; expected category " +
		                         std::to_string(static_cast<unsigned>(expected)));
	} catch (const PackageSourceError &error) {
		Require(error.Code() == expected, "package source failed with the wrong typed category");
		Require(std::string(error.what()).find("/tmp/") == std::string::npos,
		        "package source error disclosed an absolute root");
	}
}

class CountingCancellation final : public PackageCancellation {
public:
	CountingCancellation() : count(0) {
	}
	bool IsCancellationRequested() const noexcept override {
		count++;
		return false;
	}
	mutable std::uint64_t count;
};

class MutatingCancellation final : public PackageCancellation {
public:
	MutatingCancellation(std::uint64_t trigger_p, std::string target_p)
	    : trigger(trigger_p), target(std::move(target_p)), count(0), mutated(false) {
	}
	bool IsCancellationRequested() const noexcept override {
		count++;
		if (!mutated && count == trigger) {
			const int fd = ::open(target.c_str(), O_WRONLY | O_APPEND | O_CLOEXEC);
			if (fd >= 0) {
				const char byte = '\n';
				mutated = ::write(fd, &byte, 1) == 1;
				::close(fd);
			}
		}
		return false;
	}
	std::uint64_t trigger;
	std::string target;
	mutable std::uint64_t count;
	mutable bool mutated;
};

class EntryAddingCancellation final : public PackageCancellation {
public:
	EntryAddingCancellation(std::uint64_t trigger_p, std::string target_p)
	    : trigger(trigger_p), target(std::move(target_p)), count(0), mutated(false) {
	}
	bool IsCancellationRequested() const noexcept override {
		count++;
		if (!mutated && count == trigger) {
			const int fd = ::open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (fd >= 0) {
				mutated = true;
				::close(fd);
			}
		}
		return false;
	}
	std::uint64_t trigger;
	std::string target;
	mutable std::uint64_t count;
	mutable bool mutated;
};

void TestAcceptedSnapshotAndCanonicalReload() {
	TempTree tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	NeverCancel cancellation;
	const auto snapshot = AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation);
	Require(snapshot.IsValid() && snapshot.Files().size() == 5 && snapshot.RelationIds().size() == 4,
	        "accepted package did not produce the complete immutable semantic snapshot");
	Require(snapshot.RelationIds()[0] == "duckdb_login_search_page" &&
	            snapshot.RelationIds()[3] == "viewer_repository_metrics",
	        "manifest relation order was not preserved");
	Require(snapshot.Digest() == "sha256.98925899f3e110a5ed5903dae93f633061ac9800b5b613db3884b02cee65034c",
	        "accepted package snapshot has the wrong digest");
	const auto original_manifest = snapshot.Files()[0].bytes;
	WriteFile(root + "/connector.yaml", "\n", O_WRONLY | O_APPEND);
	Require(snapshot.Files()[0].bytes == original_manifest,
	        "later filesystem mutation changed an already acquired immutable snapshot");
	const auto changed = ReacquirePackageSource(snapshot, PackageSourceLimits::V1(), cancellation);
	Require(changed.Digest() != snapshot.Digest(),
	        "canonical-root reacquisition did not observe changed semantic bytes");
	const auto moved = root + "-moved";
	Require(::rename(root.c_str(), moved.c_str()) == 0, "could not rename retained package root");
	const auto after_rename = ReacquirePackageSource(snapshot, PackageSourceLimits::V1(), cancellation);
	Require(after_rename.Digest() == changed.Digest(), "retained canonical root did not survive a path rename");
}

void TestRootIndependenceAndOrdinaryFixtureIsolation() {
	TempTree tree;
	const auto first = CreateGithubPackage(tree.Path(), "first");
	const auto second = CreateGithubPackage(tree.Path(), "second");
	MakeDirectory(first + "/fixtures");
	Require(::symlink("missing", (first + "/fixtures/not-opened").c_str()) == 0,
	        "could not create fixture-only symlink");
	NeverCancel cancellation;
	const auto one = AcquirePackageSource(first, PackageSourceLimits::V1(), cancellation);
	const auto two = AcquirePackageSource(second, PackageSourceLimits::V1(), cancellation);
	Require(one.Digest() == two.Digest(), "absolute root or fixture content entered semantic identity");
}

void TestBoundaryAndOneOverLimits() {
	TempTree tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	NeverCancel cancellation;
	auto limits = PackageSourceLimits::V1();
	limits.max_root_entries = 2;
	limits.max_relation_entries = 4;
	limits.max_aggregate_entries = 6;
	limits.max_semantic_files = 5;
	const auto baseline = AcquirePackageSource(root, limits, cancellation);

	auto one_over = limits;
	one_over.max_semantic_files = 4;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
	one_over = limits;
	one_over.max_root_entries = 1;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
	one_over = limits;
	one_over.max_relation_entries = 3;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
	one_over = limits;
	one_over.max_aggregate_entries = 5;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });

	std::uint64_t aggregate_bytes = 0;
	std::uint64_t largest_file = 0;
	std::uint64_t longest_path = 0;
	for (const auto &file : baseline.Files()) {
		aggregate_bytes += file.bytes.size();
		largest_file = std::max(largest_file, static_cast<std::uint64_t>(file.bytes.size()));
		longest_path = std::max(longest_path, static_cast<std::uint64_t>(file.path.size()));
	}
	limits.max_file_bytes = largest_file;
	limits.max_aggregate_bytes = aggregate_bytes;
	limits.max_relative_path_bytes = longest_path;
	(void)AcquirePackageSource(root, limits, cancellation);
	one_over = limits;
	one_over.max_file_bytes--;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
	one_over = limits;
	one_over.max_aggregate_bytes--;
	RequireSourceFailure(PackageSourceErrorCode::RESOURCE_EXHAUSTED,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
	one_over = limits;
	one_over.max_relative_path_bytes--;
	RequireSourceFailure(PackageSourceErrorCode::INVALID_PATH,
	                     [&]() { (void)AcquirePackageSource(root, one_over, cancellation); });
}

void TestLinksSpecialFilesAndUnlistedYaml() {
	NeverCancel cancellation;
	{
		TempTree tree;
		const auto real = CreateGithubPackage(tree.Path(), "real");
		const auto link = tree.Path() + "/linked";
		Require(::symlink(real.c_str(), link.c_str()) == 0, "could not create root symlink");
		RequireSourceFailure(PackageSourceErrorCode::SYMBOLIC_LINK,
		                     [&]() { (void)AcquirePackageSource(link, PackageSourceLimits::V1(), cancellation); });
	}
	{
		TempTree tree;
		const auto root = CreateGithubPackage(tree.Path(), "github");
		const auto leaf = root + "/relations/authenticated_user.yaml";
		Require(::unlink(leaf.c_str()) == 0 && ::symlink("authenticated_repositories.yaml", leaf.c_str()) == 0,
		        "could not create relation symlink");
		RequireSourceFailure(PackageSourceErrorCode::SYMBOLIC_LINK,
		                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation); });
	}
	{
		TempTree tree;
		const auto root = CreateGithubPackage(tree.Path(), "github");
		const auto outside = tree.Path() + "/outside.yaml";
		WriteFile(outside, ReadFile("connectors/github/connector.yaml"));
		Require(::unlink((root + "/connector.yaml").c_str()) == 0 &&
		            ::link(outside.c_str(), (root + "/connector.yaml").c_str()) == 0,
		        "could not create hard-linked manifest");
		RequireSourceFailure(PackageSourceErrorCode::HARD_LINK,
		                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation); });
	}
	{
		TempTree tree;
		const auto root = CreateGithubPackage(tree.Path(), "github");
		Require(::mkfifo((root + "/pipe").c_str(), 0600) == 0, "could not create special package entry");
		RequireSourceFailure(PackageSourceErrorCode::ENTRY_TYPE,
		                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation); });
	}
	{
		TempTree tree;
		const auto root = CreateGithubPackage(tree.Path(), "github");
		WriteFile(root + "/relations/shadow.yaml", "api_version: cuac/v1\n");
		RequireSourceFailure(PackageSourceErrorCode::INVALID_PATH,
		                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation); });
	}
	{
		TempTree tree;
		const auto root = CreateGithubPackage(tree.Path(), "github");
		const auto upper = root + "/CaseProbe";
		const auto lower = root + "/caseprobe";
		WriteFile(upper, "upper");
		WriteFile(lower, "lower");
		struct stat upper_status;
		struct stat lower_status;
		if (::lstat(upper.c_str(), &upper_status) == 0 && ::lstat(lower.c_str(), &lower_status) == 0 &&
		    upper_status.st_ino != lower_status.st_ino) {
			RequireSourceFailure(PackageSourceErrorCode::INVALID_PATH,
			                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancellation); });
		}
	}
}

void TestCancellationAndMutationDetection() {
	TempTree tree;
	const auto root = CreateGithubPackage(tree.Path(), "github");
	cuac_test::AlwaysCancel cancelled;
	RequireSourceFailure(PackageSourceErrorCode::CANCELLED,
	                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), cancelled); });

	CountingCancellation counter;
	(void)AcquirePackageSource(root, PackageSourceLimits::V1(), counter);
	Require(counter.count > 4, "package source did not expose enough bounded cancellation checkpoints");
	const std::uint64_t digest_checkpoints = 6 * 5 + 2;
	MutatingCancellation mutator(counter.count - digest_checkpoints - 3,
	                             root + "/relations/viewer_repository_metrics.yaml");
	RequireSourceFailure(PackageSourceErrorCode::IDENTITY_CHANGED,
	                     [&]() { (void)AcquirePackageSource(root, PackageSourceLimits::V1(), mutator); });
	Require(mutator.mutated, "mutation test did not change the source during the acquisition interval");

	const auto second_root = CreateGithubPackage(tree.Path(), "entry-set");
	CountingCancellation second_counter;
	(void)AcquirePackageSource(second_root, PackageSourceLimits::V1(), second_counter);
	EntryAddingCancellation entry_mutator(second_counter.count - digest_checkpoints - 4,
	                                      second_root + "/added-during-acquisition");
	RequireSourceFailure(PackageSourceErrorCode::IDENTITY_CHANGED,
	                     [&]() { (void)AcquirePackageSource(second_root, PackageSourceLimits::V1(), entry_mutator); });
	Require(entry_mutator.mutated, "entry-set mutation test did not add a source during acquisition");
}

} // namespace

int main() {
	try {
		TestAcceptedSnapshotAndCanonicalReload();
		TestRootIndependenceAndOrdinaryFixtureIsolation();
		TestBoundaryAndOneOverLimits();
		TestLinksSpecialFilesAndUnlistedYaml();
		TestCancellationAndMutationDetection();
		std::cout << "package source tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "package source tests failed: " << error.what() << std::endl;
		return 1;
	}
}
