# Bounded reactive rate-limit package example

This package demonstrates a `cuac/v1` REST relation that combines the ordinary
retry recommendation with bounded, server-guided reactive rate-limit
waiting. Load the directory with
`cuac_load_connector(package_root := '/absolute/path/to/examples/rate-limit')`.

The example endpoint is illustrative and is not contacted by repository tests.
The declared `429` response is handled only after a complete response and only
before the current page is accepted or exposed. `Retry-After` supplies the
minimum eligible time; the local maxima bound each delay, cumulative waiting,
and attempts. Remaining quota and remote bucket values are observed only on a
matching response and never appear in safe explanations.
