#pragma once

#include "cuac/connector/api.hpp"
#include "support/require.hpp"

#include <cstdlib>
#include <locale>
#include <string>
#include <vector>

namespace cuac_test {
namespace scan_plan_contract {

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
	std::string canary(17, 's');
	canary.push_back('.');
	canary.append(19, 'V');
	canary.append("_semantics_plan");
	return canary;
}

class GroupedDigits final : public std::numpunct<char> {
protected:
	char do_thousands_sep() const override {
		return '_';
	}

	std::string do_grouping() const override {
		return "\1";
	}
};

class ScopedEnvironment final {
public:
	void Set(const std::string &name, const std::string &value) {
		const char *previous = std::getenv(name.c_str());
		entries.push_back(Entry {name, previous != nullptr, previous ? previous : ""});
		Require(setenv(name.c_str(), value.c_str(), 1) == 0, "could not install hostile planner environment");
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

inline const cuac::CompiledRelation &FindRelation(const cuac::CompiledConnector &connector,
                                                  const std::string &exact_relation_name) {
	const auto *selected = connector.FindRelation(exact_relation_name);
	Require(selected != nullptr, "fixture is missing the exact relation identifier");
	return *selected;
}

} // namespace scan_plan_contract
} // namespace cuac_test
