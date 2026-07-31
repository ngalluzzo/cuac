#include "connector/support/catalog_contract.hpp"
#include "connector/support/pagination_contract.hpp"
#include "connector/support/predicate_contract.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
	try {
		cuac_test::RunConnectorCatalogContractTests();
		cuac_test::RunConnectorPaginationContractTests();
		cuac_test::RunConnectorPredicateContractTests();
		std::cout << "connector model contract tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "connector model contract tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
