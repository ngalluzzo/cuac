# Development environment

CUAC separates portable development verification from native release evidence.
They answer different questions and must not silently stand in for one another.

## Portable verification authority

`containers/development/Dockerfile` is the canonical compiler and dependency
environment for ordinary development and CI. It uses a multi-architecture
Debian base pinned by digest and package repositories frozen at one Debian
snapshot timestamp. `containers/development/toolchain.json` records the
independent identity contract checked at repository and container runtime.
The slim base bootstraps the frozen APT snapshot over HTTP because it does not
yet contain a CA bundle; APT verifies the signed Debian metadata and installs
the snapshot-pinned CA package before any other network tooling is used.

The image runs natively as `linux/amd64` or `linux/arm64`. Both architectures
use the same Dockerfile, dependency snapshot, DuckDB source identities, build
commands, and tests. Architecture emulation is not required for normal work.

The three consumers are deliberately thin:

- The root Makefile enters the image when invoked on a host.
- `.devcontainer/devcontainer.json` opens the same image as a workspace.
- `.github/workflows/repository-contracts.yml` builds the same image and runs
  `make verify` inside it.

Inside the verified container, `CUAC_DEV_CONTAINER=1` is a trust-boundary marker,
not proof by itself. The environment verifier also checks the base operating
system, architecture, compiler, CMake, Ninja, Python, clang-format, and libcurl
identities before a build begins. The formatter is installed from hash-locked
multi-architecture wheels so both container architectures enforce the same
formatting version.

## Native release evidence

DuckDB loadable extensions are platform-specific binaries. A Linux container
therefore cannot certify macOS or Windows linkage, loader behavior, SDK inputs,
or artifact compatibility.

Native release runners are named for their platform. The current macOS runner
is `scripts/run-macos-product-tests.sh`; it verifies the exact macOS SDK and
libcurl identity recorded under `release_cells.macos_arm64` in the release pin
file. It is a release lane, not the development default. Additional native
release cells must get their own dependency authority and artifact evidence
before CUAC claims support for them.

## Commands

From a host with Docker:

```sh
make image
make build
make test
make verify
make shell
```

From inside the Dev Container, use the same commands. The router detects the
verified-container boundary and executes directly instead of nesting Docker.

From a Docker host, generated state is stored in a per-checkout named volume
mounted at `/var/lib/cuac-dev`; a Dev Container uses the same mount point with
its own persistent volume. Keeping object files, archives, and linker output
outside the bind-mounted workspace avoids host-filesystem translation costs on
macOS and Windows. The host router prints the exact volume name as
`developer_state_volume`. `CUAC_DEV_VOLUME` may override that name when a
separately managed volume is required.

`make build`, `make test`, and `make demo` reuse the `container` build cell and
its explicitly configured `ccache` for the development loop. `make verify`
allocates a new build root and a new compiler cache under the Linux-backed
state volume, runs the complete product suite, and removes the temporary tree
when it exits. Neither path is native release evidence.

Successful commands emit `timing phase=... elapsed_seconds=...` records for
environment preparation, source preflight, native compilation, each broad test
layer, image construction, and the complete container command. Native builds
also report direct cache hits, preprocessed hits, misses, and cache size.
