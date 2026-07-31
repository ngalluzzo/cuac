# Bounded retry package example

This package demonstrates the smallest `cuac/v1` REST relation that opts
into bounded replay-safe retry. Load the directory with
`cuac_load_connector(package_root := '/absolute/path/to/examples/retry')`.

The example endpoint is illustrative and is not contacted by repository tests.
The declaration does not make an operation retryable by itself: the compiler must prove the
complete operation is a replayable read, and Runtime may retry only before the
current page is accepted or exposed.
