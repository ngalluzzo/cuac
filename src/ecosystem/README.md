# Package ecosystem

This context governs a compiled connector package after Connector Experience
has produced it and before Query Experience activates it. The current boundary
is deliberately small: `reload/` classifies whether one immutable `cuac/v1`
generation may replace another.

It does not parse package source, compile connector metadata, publish DuckDB
catalog entries, or execute remote requests. Future distribution, registry,
lock, and provenance capabilities belong here only when they have production
behavior; empty placeholder modules are not created.

The public decision contract is
[`package_compatibility.hpp`](../include/cuac/ecosystem/package_compatibility.hpp).
