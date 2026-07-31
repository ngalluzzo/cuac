#include "query/support/duckdb_secret_test_support.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
	using namespace cuac_test::duckdb_secret;
	try {
		RunTest("registered surface", TestRegisteredSurfaceCoversAllProvidersAndStorageModes);
		RunTest("creation validation", TestCreationRejectsImplicitPersistenceAndMalformedOptions);
		RunTest("resolution validation", TestResolutionRejectsMissingGenericAndAmbiguousEntries);
		RunTest("identity lifecycle", TestConfigAndEnvironmentIdentityLifecycles);
		RunTest("current transaction", TestTemporaryResolutionUsesCurrentTransaction);
		RunTest("temporary without persistence", TestTemporaryResolutionDoesNotRequirePersistentStorage);
		RunTest("persistent restart", TestPersistentResolutionSurvivesRestartAndRequiresAutocommit);
		RunTest("persistent record bound", TestPersistentStorageEnforcesLiveRecordBound);
		RunTest("persistent cancellation and shutdown", TestPersistentResolutionCancellationAndShutdown);
		RunTest("persistent failure atomicity", TestPersistentMutationFailureAtomicity);
		RunTest("excluded storage", TestResolutionDoesNotQueryExcludedStorages);
		RunTest("registration failure", TestFailedProviderRegistrationNeverPublishesScan);
		std::cout << "duckdb secret tests passed" << std::endl;
		return EXIT_SUCCESS;
	} catch (const std::exception &error) {
		std::cerr << "duckdb secret tests failed: " << error.what() << std::endl;
		return EXIT_FAILURE;
	}
}
