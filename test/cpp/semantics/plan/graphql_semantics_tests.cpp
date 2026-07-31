#include "semantics/support/graphql_semantics_test_cases.hpp"

#include <exception>
#include <iostream>

int main(int argc, char **argv) {
	if (argc != 2) {
		std::cerr << "usage: graphql_semantics_tests ABSOLUTE_REPOSITORY_ROOT" << std::endl;
		return 1;
	}
	try {
		cuac_test::graphql_semantics::TestCursorResources(argv[1]);
		cuac_test::graphql_semantics::TestNullability(argv[1]);
		cuac_test::graphql_semantics::TestPackageGraphqlPlanning(argv[1]);
		std::cout << "GraphQL semantics tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "GraphQL semantics tests failed: " << error.what() << std::endl;
		return 1;
	}
}
