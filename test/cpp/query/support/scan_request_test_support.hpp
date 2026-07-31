#pragma once

#include "cuac/query/scan_request.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace cuac_test {
namespace query_scan_request {

// Shared Query-owned mechanics for the request contract oracle. Case
// narratives remain in scan_request_tests.cpp; this support owns only bounded
// exception assertions, environment restoration, source preflight, and
// exhaustive request-field inspection.
template <class EXCEPTION, class ACTION>
void RequireThrows(const ACTION &action, const std::string &message) {
	bool rejected = false;
	try {
		action();
	} catch (const EXCEPTION &) {
		rejected = true;
	}
	Require(rejected, message);
}

inline std::string RuntimeCredentialCanary() {
	std::string canary(11, 'q');
	canary.push_back('.');
	canary.append(13, 'T');
	canary.append("_query_request");
	return canary;
}

inline std::string ReadText(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	Require(input.good(), "could not preflight request source: " + path);
	return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

class ScopedEnvironment final {
public:
	void Set(const std::string &name, const std::string &value) {
		const char *previous = std::getenv(name.c_str());
		entries.push_back(Entry {name, previous != nullptr, previous ? previous : ""});
		Require(setenv(name.c_str(), value.c_str(), 1) == 0, "could not install hostile request environment");
	}

	~ScopedEnvironment() {
		for (auto entry = entries.rbegin(); entry != entries.rend(); ++entry) {
			if (entry->present) {
				(void)setenv(entry->name.c_str(), entry->value.c_str(), 1);
			} else {
				(void)unsetenv(entry->name.c_str());
			}
		}
	}

private:
	struct Entry {
		std::string name;
		bool present;
		std::string value;
	};
	std::vector<Entry> entries;
};

inline void RequireFullSelectedSchema(const cuac::ScanRequest &request, const cuac::CompiledRelation &relation) {
	Require(request.projected_columns.size() == relation.Columns().size(),
	        "request did not copy the complete selected schema");
	for (std::size_t index = 0; index < relation.Columns().size(); index++) {
		Require(request.projected_columns[index] == relation.Columns()[index].name,
		        "request schema did not preserve selected column order and identity");
	}
}

inline void RequireCanaryAbsent(const cuac::ScanRequest &request, const std::string &canary) {
	Require(request.connector_name.find(canary) == std::string::npos &&
	            request.relation_name.find(canary) == std::string::npos &&
	            request.requested_predicate.Snapshot().find(canary) == std::string::npos,
	        "credential canary entered scalar request identity or predicate");
	for (const auto &value : request.explicit_inputs) {
		Require(value.Identifier().find(canary) == std::string::npos &&
		            (value.IsNull() || value.Kind() != cuac::ExplicitInputValueKind::VARCHAR ||
		             value.VarcharValue().find(canary) == std::string::npos),
		        "credential canary entered request explicit inputs");
	}
	for (const auto &value : request.projected_columns) {
		Require(value.find(canary) == std::string::npos, "credential canary entered request projection");
	}
	for (const auto &value : request.orderings) {
		Require(value.find(canary) == std::string::npos, "credential canary entered request ordering");
	}
	Require(request.secret_reference.Snapshot().find(canary) == std::string::npos &&
	            request.Snapshot().find(canary) == std::string::npos,
	        "credential canary entered a request snapshot");
	if (request.secret_reference.IsPresent()) {
		Require(request.secret_reference.Name().find(canary) == std::string::npos,
		        "credential canary entered the logical secret selector");
	}
}

} // namespace query_scan_request
} // namespace cuac_test
