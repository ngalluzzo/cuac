#include "cuac/internal/runtime/pagination/uri_reference.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using cuac::internal::IsValidUri;
using cuac::internal::IsValidUriReference;
using cuac_test::Require;

void TestValidReferences() {
	const std::vector<std::string> references = {
	    "",
	    "relative/path",
	    "../page?field=value#fragment",
	    "/absolute/path",
	    "//example.test/path",
	    "https://example.test/path?field=value#fragment",
	    "https://[2001:db8::1]/path",
	    "https://[::ffff:192.0.2.1]/path",
	    "https://[v1.fe80::a]/path",
	    "urn:example:repository",
	};
	for (const auto &reference : references) {
		Require(IsValidUriReference(reference), "valid URI-reference was rejected: " + reference);
	}
	Require(IsValidUri("https://example.test/path"), "hierarchical URI was rejected");
	Require(IsValidUri("urn:example:repository"), "rootless URI was rejected");
}

void TestInvalidReferences() {
	const std::vector<std::string> references = {
	    "bad reference",
	    "%",
	    "1relative:first",
	    "//[2001:db8::1/path",
	    "//[not-ip]/path",
	    "//[2001:db8:::1]/path",
	    "//[::ffff:192.0.2.999]/path",
	    "//[v1.]/path",
	    "//first@second@example.test/path",
	    "//example.test:not-a-port/path",
	    "//example.test:443:444/path",
	    "relative/path#first#second",
	};
	for (const auto &reference : references) {
		Require(!IsValidUriReference(reference), "malformed URI-reference was accepted: " + reference);
	}
	Require(!IsValidUri("relative/path"), "relative reference was accepted as a URI");
	Require(!IsValidUri("//example.test/path"), "network-path reference was accepted as a URI");
}

} // namespace

int main() {
	try {
		TestValidReferences();
		TestInvalidReferences();
		std::cout << "URI-reference tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "URI-reference tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
