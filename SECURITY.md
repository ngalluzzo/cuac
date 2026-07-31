# Security policy

This policy applies to the current CUAC source preview and its latest published
release.

## Supported versions

Only the latest published release is supported. There is no long-term-support
branch and no backport policy: a fix ships in the next release built from the
latest source, not as a patch to an older version.

## Reporting a vulnerability

Report most issues, including most security issues, as an ordinary
[GitHub Issue](https://github.com/ngalluzzo/cuac/issues). There is no
private disclosure channel or committed response SLA beyond the project's
existing best-effort support.

If reproducing the issue would require posting a live hostname, credential,
token, or full request/response body (most likely for transport,
authentication, network-policy, or credential-handling issues), do not paste
that detail into a public issue. Redact hostnames, tokens, and request/
response bodies and describe the flaw abstractly instead.

If the issue cannot be described without that sensitive detail, use GitHub's
private vulnerability reporting instead of a public issue: open this
repository's **Security** tab and select **Report a vulnerability**. This adds
no new response-time commitment beyond the same best-effort basis as ordinary
issues — it only keeps sensitive reproduction detail out of public view.
