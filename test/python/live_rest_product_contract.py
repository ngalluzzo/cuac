#!/usr/bin/env python3

"""Run the private direct-load live REST product contract."""

from __future__ import annotations

import json
import pathlib
import sys


# Python isolated mode intentionally omits the script directory. Add only this
# repository-owned test package, never ambient PYTHONPATH or site state.
TEST_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_ROOT))

from live_rest_product.lifecycle import run_lifecycle_contract  # noqa: E402
from live_rest_product.relational import run_relational_contract  # noqa: E402
from live_rest_product.support import (  # noqa: E402
    configure_controlled_environment,
    start_oracle,
    stop_oracle,
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: live_rest_product_contract.py PATH_TO_CONTROLLED_EXTENSION"
        )

    extension_path = pathlib.Path(sys.argv[1]).resolve(strict=True)
    server, server_thread = start_oracle()
    try:
        configure_controlled_environment(server)
        relational_requests = run_relational_contract(extension_path, server)
        total_requests = run_lifecycle_contract(extension_path, server)
    finally:
        stop_oracle(server, server_thread)

    print(
        json.dumps(
            {
                "artifact": extension_path.name,
                "lifecycle_requests": total_requests - relational_requests,
                "relational_requests": relational_requests,
                "requests": total_requests,
                "status": "ok",
            },
            sort_keys=True,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
