#include "cuac/internal/runtime/policy/request_validation.hpp"
#include "support/require.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

using cuac_test::Require;

void TestCanonicalRequestLiteralGrammar() {
	using cuac::internal::IsSafeEncodedQueryValue;
	Require(IsSafeEncodedQueryValue("plain-._~") && IsSafeEncodedQueryValue("hello+world") &&
	            IsSafeEncodedQueryValue("%2F%2B") && IsSafeEncodedQueryValue("%E2%82%AC"),
	        "canonical form-urlencoded values were rejected");
	Require(!IsSafeEncodedQueryValue("raw/value") && !IsSafeEncodedQueryValue("%41") &&
	            !IsSafeEncodedQueryValue("%20") && !IsSafeEncodedQueryValue("%2f") && !IsSafeEncodedQueryValue("%00") &&
	            !IsSafeEncodedQueryValue("%C2%80") && !IsSafeEncodedQueryValue("%C2%9F") &&
	            !IsSafeEncodedQueryValue("%C0%AF"),
	        "noncanonical or invalid form-urlencoded values were admitted");
}

void TestCanonicalStructuralRequestPathGrammar() {
	using cuac::internal::IsSafeRequestPath;
	Require(IsSafeRequestPath("/") && IsSafeRequestPath("/repos/open%20ai/cuac%20%CE%B2/issues") &&
	            IsSafeRequestPath("/projects/-42/issues/7"),
	        "canonical structural request paths were rejected");
	const std::vector<std::string> rejected = {
	    "repos/openai",      "/repos//issues",          "/repos/./issues",
	    "/repos/../issues",  "/repos/%41/issues",       "/repos/%2D/issues",
	    "/repos/%2f/issues", "/repos/%2F/issues",       "/repos/%5C/issues",
	    "/repos/%3F/issues", "/repos/%23/issues",       "/repos/%25/issues",
	    "/repos/%2E/issues", "/repos/%/issues",         "/repos/%2/issues",
	    "/repos/%GG/issues", "/repos/raw value/issues", std::string("/repos/raw/\xCE\xB2")};
	for (std::size_t index = 0; index < rejected.size(); index++) {
		Require(!IsSafeRequestPath(rejected[index]),
		        "noncanonical structural request path was admitted at index " + std::to_string(index));
	}
}

void TestNumericIpv4AliasesAreNotDnsHosts() {
	using cuac::internal::IsSafeDnsHost;
	const char *const numeric_aliases[] = {
	    "0",          "2130706433",   "0x7f000001",      "0X7F000001",   "4294967296",    "0x100000000",
	    "9999999999", "017700000001", "127.1",           "0x7f.1",       "0X7f.1",        "0177.01",
	    "127.0.1",    "0x7f.0.1",     "0177.0.01",       "127.0.0.1",    "0x7f.0.0.1",    "0177.0.0.01",
	    "134744072",  "0x08080808",   "010.010.010.010", "255.16777215", "255.255.65535", "255.255.255.255",
	    "4294967295", "0xffffffff",   "0XFFFFFFFF"};
	for (const auto *alias : numeric_aliases) {
		Require(!IsSafeDnsHost(alias), std::string("numeric IPv4 alias was admitted as DNS: ") + alias);
	}

	const char *const dns_hosts[] = {"api.example.com", "example123", "deadbeef", "09", "0x",
	                                 "256.1.1.1",       "1.16777216", "1.1.65536"};
	for (const auto *host : dns_hosts) {
		Require(IsSafeDnsHost(host), std::string("legitimate DNS host was classified as IPv4: ") + host);
	}
}

void TestStructuralPathBoundaries() {
	using cuac::internal::IsGraphqlName;
	using cuac::internal::IsSafeGraphqlPath;
	using cuac::internal::IsSafeRestCollectionPath;
	using cuac::internal::IsSafeRestExtractPath;

	const std::vector<std::string> sixteen(16, "field");
	const std::vector<std::string> seventeen(17, "field");
	Require(IsSafeRestExtractPath(sixteen) && IsSafeRestExtractPath(seventeen) && IsSafeRestCollectionPath(sixteen) &&
	            IsSafeRestCollectionPath(seventeen),
	        "REST structural paths inherited GraphQL's root segment bound");
	Require(IsSafeRestExtractPath({std::string(1022, 'a')}) && !IsSafeRestExtractPath({std::string(1023, 'a')}) &&
	            IsSafeRestCollectionPath({std::string(1022, 'a')}) &&
	            !IsSafeRestCollectionPath({std::string(1023, 'a')}),
	        "REST extract or collection spelling did not enforce its exact RFC byte ceiling");
	Require(IsSafeGraphqlPath(std::vector<std::string>(14, "field"), 1, 16) &&
	            IsSafeGraphqlPath(std::vector<std::string>(15, "field"), 1, 16) && IsSafeGraphqlPath(sixteen, 1, 16) &&
	            !IsSafeGraphqlPath(seventeen, 1, 16) &&
	            IsSafeGraphqlPath(std::vector<std::string>(18, "field"), 3, 18) &&
	            !IsSafeGraphqlPath(std::vector<std::string>(19, "field"), 3, 18) &&
	            IsSafeGraphqlPath(std::vector<std::string>(19, "field"), 4, 19) &&
	            !IsSafeGraphqlPath(std::vector<std::string>(20, "field"), 4, 19),
	        "GraphQL root, response, or cursor role bounds drifted");
	Require(IsGraphqlName(std::string(255, 'a')) && !IsGraphqlName(std::string(256, 'a')) &&
	            !IsGraphqlName("__reserved"),
	        "GraphQL Name grammar drifted from the package contract");
}

void TestSignedPageSequenceBoundaries() {
	using cuac::internal::IsSignedBigintPageSequence;
	const auto maximum = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
	Require(IsSignedBigintPageSequence(maximum, 1, 1) && IsSignedBigintPageSequence(maximum - 1, 1, 2) &&
	            !IsSignedBigintPageSequence(maximum, 1, 2) && !IsSignedBigintPageSequence(maximum - 1, 2, 2) &&
	            !IsSignedBigintPageSequence(maximum, 0, 1),
	        "paginated REST sequence escaped the signed BIGINT contract");
}

} // namespace

int main() {
	try {
		TestCanonicalRequestLiteralGrammar();
		TestCanonicalStructuralRequestPathGrammar();
		TestNumericIpv4AliasesAreNotDnsHosts();
		TestStructuralPathBoundaries();
		TestSignedPageSequenceBoundaries();
		std::cout << "Request validation tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "Request validation tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
