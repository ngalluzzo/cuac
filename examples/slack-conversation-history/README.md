# Slack conversation history (independent-author example)

This package is [RFC 0029](../../docs/rfcs/0029-admit-response-cursor-continuation.md)'s
evidence that `response_cursor` pagination is reachable by an author working
from the published specification alone. It is **not** a maintained provider: it
lives outside `connectors/`, so the maintained example providers remain GitHub
and Rick and Morty.

Slack's `conversations.history` is the corpus relation that motivated the
capability. It returns an opaque `response_metadata.next_cursor` token that must
be echoed into the next request's `cursor` query parameter — there is no
`Link` header, no next-page URL, and no page number, so none of the four
earlier REST strategies can express it.

```yaml
pagination:
  strategy: response_cursor
  dependency: sequential
  consistency: mutable
  target_scope: exact_operation_origin_and_path
  cursor_path: $.response_metadata.next_cursor
  cursor_parameter: cursor
  max_cursor_bytes: 512
  max_pages_per_scan: 16
```

The first request omits `cursor` entirely; every later request carries the
received token once, percent-encoded by Runtime. An absent path, a JSON `null`,
and an empty string all mean "no next page".

## What this package proves

`cuac_slack_independent_package_tests` compiles this root and asserts that it

- publishes exactly one `response_cursor` relation;
- derives its complete 95-key coverage matrix, including all sixteen
  `pagination_*` cursor keys — no other package in the tree derives them; and
- clears fixture schema validation, exact claimed-coverage agreement, and exact
  payload identity **before** the runner enters any execution service.

No package-specific native code exists for it. The fixtures are deterministic
local payloads; the live Slack API is never a correctness oracle.

## Known limitation

Slack reports application errors as HTTP 200 with `{"ok": false, "error": ...}`.
`cuac/v1` has no envelope-status declaration, so a revoked token surfaces as a
decode failure when `$.messages[*]` is absent rather than as an authentication
diagnostic. It fails closed, but the diagnostic is less specific than it should
be. RFC 0029 records this as explicitly deferred — no RFC 0028 classification
covers an envelope-status field yet.
