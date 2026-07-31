#pragma once

#include "cuac/internal/connector/compiler/package_cancellation.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace cuac_test {

class NeverCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return false;
	}
};

class AlwaysCancel final : public cuac::connector::PackageCancellation {
public:
	bool IsCancellationRequested() const noexcept override {
		return true;
	}
};

inline void Require(bool condition, const std::string &message) {
	if (!condition) {
		throw std::runtime_error(message);
	}
}

inline std::string ReadFile(const std::string &path) {
	std::ifstream input(path.c_str(), std::ios::binary);
	if (!input) {
		throw std::runtime_error("could not read test fixture: " + path);
	}
	std::ostringstream bytes;
	bytes << input.rdbuf();
	return bytes.str();
}

} // namespace cuac_test
