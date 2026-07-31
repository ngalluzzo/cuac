#!/usr/bin/env python3

"""Run the private direct-load authenticated-relation product contract."""

from __future__ import annotations

import json
import pathlib
import sys


TEST_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_ROOT))

from authenticated_relation_product.relational import run_success_contract  # noqa: E402
from authenticated_relation_product.failures import run_failure_contract  # noqa: E402
from authenticated_relation_product.lifecycle import run_lifecycle_contract  # noqa: E402
from authenticated_relation_product.secrets import run_secret_contract  # noqa: E402
from live_rest_product.support import (  # noqa: E402
    configure_controlled_environment,
    start_oracle,
    stop_oracle,
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: authenticated_relation_product_contract.py "
            "PATH_TO_CONTROLLED_EXTENSION"
        )

    extension_path = pathlib.Path(sys.argv[1]).resolve(strict=True)
    server, server_thread = start_oracle()
    try:
        configure_controlled_environment(server)
        success_requests = run_success_contract(extension_path, server)
        secret_requests = run_secret_contract(extension_path, server)
        failure_requests = run_failure_contract(extension_path, server)
        requests = run_lifecycle_contract(extension_path, server)
    finally:
        stop_oracle(server, server_thread)

    print(
        json.dumps(
            {
                "artifact": extension_path.name,
                "failure_requests": failure_requests - secret_requests,
                "lifecycle_requests": requests - failure_requests,
                "requests": requests,
                "secret_requests": secret_requests - success_requests,
                "status": "ok",
                "success_requests": success_requests,
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
