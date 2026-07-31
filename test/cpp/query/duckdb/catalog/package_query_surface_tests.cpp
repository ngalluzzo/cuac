#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace cuac_test {
void RunGeneratedRelationTests();
void RunGithubPackageSurfaceTests(const std::string &absolute_repository_root);
void RunRickAndMortyPackageSurfaceTests(const std::string &absolute_repository_root);
void RunPackageIntrospectionTests();
void RunPackageLifecycleTests();
void RunPackageManagementTests();
void RunPackagePublicationCancellationTests();
} // namespace cuac_test

int main(int argc, char **argv) {
	try {
		if (argc != 2 || argv[1][0] != '/') {
			throw std::invalid_argument("usage: package_query_surface_tests ABSOLUTE_REPOSITORY_ROOT");
		}
		cuac_test::RunPackageManagementTests();
		cuac_test::RunPackageIntrospectionTests();
		cuac_test::RunGeneratedRelationTests();
		cuac_test::RunPackageLifecycleTests();
		cuac_test::RunPackagePublicationCancellationTests();
		cuac_test::RunGithubPackageSurfaceTests(argv[1]);
		cuac_test::RunRickAndMortyPackageSurfaceTests(argv[1]);
		std::cout << "cuac package Query surface tests passed" << std::endl;
		return 0;
	} catch (const std::exception &error) {
		std::cerr << "cuac package Query surface tests failed: " << error.what() << std::endl;
		return 1;
	}
}
