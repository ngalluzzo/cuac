#pragma once

#include <exception>
#include <string>
#include <vector>

namespace cuac {
namespace internal {

// Generic RFC 8288 field-value grammar. The parser validates every target,
// relation token, anchor, and extension parameter before visiting the value;
// it neither selects a continuation nor grants request authority. The target
// reference is call-scoped and must not be retained except under a consuming
// policy's separate resource and authority rules.
class LinkHeaderValueVisitor {
public:
	virtual ~LinkHeaderValueVisitor() noexcept;
	virtual void Visit(const std::string &target, bool has_next_relation) = 0;
};

class LinkHeaderSyntaxError : public std::exception {
public:
	const char *what() const noexcept override;
};

// Parses physical Link field-values in receipt order. A bounded number of
// empty RFC list elements is ignored. Syntax failures expose no received value
// or target; allocation failures remain owned by the caller.
void ParseLinkHeaderFields(const std::vector<std::string> &fields, LinkHeaderValueVisitor &visitor);

} // namespace internal
} // namespace cuac
