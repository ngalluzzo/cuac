#pragma once

#include <stdexcept>
#include <string>

namespace cuac_test {

inline void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

} // namespace cuac_test
