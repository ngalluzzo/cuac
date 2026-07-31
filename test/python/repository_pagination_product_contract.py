#!/usr/bin/env python3

"""Run the private direct-load repository-pagination product contract."""

from __future__ import annotations

import json
import pathlib
import sys


TEST_ROOT = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(TEST_ROOT))

from repository_pagination_product.composition import (  # noqa: E402
    run_composition_differential,
)
from repository_pagination_product.failures import run_failure_contract  # noqa: E402
from repository_pagination_product.lifecycle import run_lifecycle_contract  # noqa: E402
from repository_pagination_product.relational import run_relational_contract  # noqa: E402
from repository_pagination_product.service import start_oracle, stop_oracle  # noqa: E402
from repository_pagination_product.support import (  # noqa: E402
    configure_controlled_environment,
    load_repository_connection,
)


def main() -> int:
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: repository_pagination_product_contract.py "
            "PATH_TO_CONTROLLED_EXTENSION"
        )

    extension_path = pathlib.Path(sys.argv[1]).resolve(strict=True)
    server, server_thread = start_oracle()
    try:
        configure_controlled_environment(server)
        composition_start = server.lifetime_request_count()
        optimized = load_repository_connection(extension_path, server)
        try:
            run_composition_differential(extension_path, server, optimized)
        finally:
            optimized.close()
        composition_requests = server.lifetime_request_count() - composition_start

        relational_start = server.lifetime_request_count()
        run_relational_contract(extension_path, server)
        relational_requests = server.lifetime_request_count() - relational_start

        failure_start = server.lifetime_request_count()
        run_failure_contract(extension_path, server)
        failure_requests = server.lifetime_request_count() - failure_start

        lifecycle_start = server.lifetime_request_count()
        run_lifecycle_contract(extension_path, server)
        lifecycle_requests = server.lifetime_request_count() - lifecycle_start
        total_requests = server.lifetime_request_count()
    finally:
        stop_oracle(server, server_thread)

    print(
        json.dumps(
            {
                "artifact": extension_path.name,
                "composition_requests": composition_requests,
                "failure_requests": failure_requests,
                "lifecycle_requests": lifecycle_requests,
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
