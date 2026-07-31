# Public surface inventory

`inventory.json` is the canonical content-addressed inventory of CUAC-owned SQL
table functions for the initial `0.1.0` contract. `query-contract.json` is the
independent exact projection consumed by Query tests, and
`inventory.schema.json` closes the editable document shape.

The initial baseline contains only currently active functions. Historical
prototype names and transitions are intentionally absent because they were
never part of a CUAC release.

Run:

```sh
python3 -I -B scripts/verify-public-surface-inventory.py
python3 -I -B test/python/public_surface_inventory_tests.py
```

Every future SQL addition, change, deprecation, or removal must add an ordered
revision and matching release view. Shape identifiers are hashes of their
canonical contents; never edit a shape without assigning its new digest.
