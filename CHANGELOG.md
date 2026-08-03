# Changelog

## [0.2.0](https://github.com/ngalluzzo/cuac/compare/v0.1.0...v0.2.0) (2026-08-03)


### Features

* **rest:** add typed structural path segments ([e0c6113](https://github.com/ngalluzzo/cuac/commit/e0c61132626421aa4b76816508667efd97e38ada))
* **runtime:** certify bounded resilience behavior ([41c6be4](https://github.com/ngalluzzo/cuac/commit/41c6be409fd51d84d80b7934dd96609bbd92b7b9))
* **types:** add TIMESTAMPTZ scalar support ([4281344](https://github.com/ngalluzzo/cuac/commit/42813449bd04b70a566992171d84b31c8d01f3eb))


### Bug Fixes

* **ci:** preserve executable verification tools ([9c96905](https://github.com/ngalluzzo/cuac/commit/9c96905921ae82e8d04739aa6383b6825cf804a3))
* **ci:** verify committed source identities ([c9328e0](https://github.com/ngalluzzo/cuac/commit/c9328e0afb1832b8798c83d66e97e8b662dbefa2))


### Performance Improvements

* **build:** keep container state on Linux volumes ([fae8d24](https://github.com/ngalluzzo/cuac/commit/fae8d248b01a215b447dda27ab814f551f8e4369))

## 0.1.0 (2026-08-01)


### Features

* **connector:** acquire versioned package sources ([15ecfdf](https://github.com/ngalluzzo/cuac/commit/15ecfdf1076b9f3b11dfead3fc1cffbd73888ee6))
* **connector:** compile local packages ([f2133a2](https://github.com/ngalluzzo/cuac/commit/f2133a281e510f2605c2cdcb73cf43f4400541b6))
* **connector:** define immutable package metadata ([aaf0ad4](https://github.com/ngalluzzo/cuac/commit/aaf0ad424d04e3f58ef03a072b177d2a5ceadb69))
* **connectors:** add reference connector packages ([df25b2f](https://github.com/ngalluzzo/cuac/commit/df25b2f5b8dcabb194d37206bdf7b377aec63724))
* **connector:** validate offline package fixtures ([ad01980](https://github.com/ngalluzzo/cuac/commit/ad019806796047f034b55dee0c06cede0d5a9941))
* **ecosystem:** classify package reload compatibility ([841f63b](https://github.com/ngalluzzo/cuac/commit/841f63bf1e51a3e7710111f69d8c0260ad250bc2))
* **query:** manage DuckDB credentials ([706c62c](https://github.com/ngalluzzo/cuac/commit/706c62c925049224271c38943bb360d4b3f11e3d))
* **query:** model protocol-neutral scan requests ([54bf2c1](https://github.com/ngalluzzo/cuac/commit/54bf2c16d15202fd9e0402745520d14f55477dd3))
* **query:** publish connector relations in DuckDB ([2a652d2](https://github.com/ngalluzzo/cuac/commit/2a652d2e681001bd1970782f885c074ff279b2db))
* **query:** publish scan profiles in explain analyze ([73692fb](https://github.com/ngalluzzo/cuac/commit/73692fb61dff75a1b9c2f8bf0d65160d0a029c0e))
* **runtime:** add resilient generation lifecycle ([a422cdb](https://github.com/ngalluzzo/cuac/commit/a422cdb57547657064d27d8c9bd5552e4e052103))
* **runtime:** define bounded execution contracts ([f3ed72f](https://github.com/ngalluzzo/cuac/commit/f3ed72f857af017b4a8f94af2edd2eaf2d456410))
* **runtime:** execute bounded paginated scans ([052b988](https://github.com/ngalluzzo/cuac/commit/052b988fa94f08c38b5c4973846a99f92c9930ec))
* **runtime:** expose bounded terminal scan profiles ([91a675d](https://github.com/ngalluzzo/cuac/commit/91a675d9bc2f9743c058f662999662cde159e61d))
* **runtime:** transport and decode remote responses ([74c9872](https://github.com/ngalluzzo/cuac/commit/74c98728ad8af435ed3586339cf282a359df473d))
* **semantics:** define relational plan values ([95ab2e1](https://github.com/ngalluzzo/cuac/commit/95ab2e141293397f9ccef6e97f88e3fedb010cac))
* **semantics:** plan connector operations ([56cbc3e](https://github.com/ngalluzzo/cuac/commit/56cbc3ed1fbfa6ed1dabcc5f4622eb8345aa19f9))


### Bug Fixes

* **cache:** preserve terminal cache stream semantics ([91a50ae](https://github.com/ngalluzzo/cuac/commit/91a50ae4d315012ff1f31e706bbacbb10c251da8))
* **release:** verify the portable 0.1.0 baseline ([c839780](https://github.com/ngalluzzo/cuac/commit/c83978009247c15aaf7a178f5d9b8486a0c19c71))
