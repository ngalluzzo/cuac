PROJ_DIR := $(CURDIR)/

EXT_NAME=cuac
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Keep discovery relative to CURDIR so GNU Make does not split an absolute
# include path when a checkout directory contains spaces. Supported entry
# points invoke this Makefile from its own directory, directly or with `-C`.
EXTENSION_TEMPLATE_MAKEFILE := extension-ci-tools/makefiles/duckdb_extension.Makefile
override CUAC_DEVELOPMENT_GOALS := help bootstrap build test demo paths verify shell image
override CUAC_REQUESTED_GOALS := $(strip $(MAKECMDGOALS))
override CUAC_REQUESTED_DEVELOPMENT_GOALS := $(filter $(CUAC_DEVELOPMENT_GOALS),$(CUAC_REQUESTED_GOALS))
override CUAC_REQUESTED_UPSTREAM_GOALS := $(filter-out $(CUAC_DEVELOPMENT_GOALS),$(CUAC_REQUESTED_GOALS))

ifneq ($(CUAC_REQUESTED_DEVELOPMENT_GOALS),)
ifneq ($(CUAC_REQUESTED_UPSTREAM_GOALS),)
$(error development goal(s) $(CUAC_REQUESTED_DEVELOPMENT_GOALS) cannot be combined with Community/upstream goal(s) $(CUAC_REQUESTED_UPSTREAM_GOALS))
endif
endif

ifneq ($(CUAC_REQUESTED_UPSTREAM_GOALS),)
ifeq ($(wildcard $(EXTENSION_TEMPLATE_MAKEFILE)),)
$(error Community/upstream goal(s) $(CUAC_REQUESTED_UPSTREAM_GOALS) require an initialized extension-ci-tools submodule)
endif
include $(EXTENSION_TEMPLATE_MAKEFILE)
else

.DEFAULT_GOAL := help

PROFILE ?= debug
DEVELOPMENT := $(PROJ_DIR)scripts/development.sh

.PHONY: help bootstrap build test demo paths verify shell image

help:
	@"$(DEVELOPMENT)" help

bootstrap:
	@"$(DEVELOPMENT)" bootstrap

build:
	@"$(DEVELOPMENT)" build "$(PROFILE)"

test:
	@"$(DEVELOPMENT)" test "$(PROFILE)"

demo:
	@"$(DEVELOPMENT)" demo "$(PROFILE)"

paths:
	@"$(DEVELOPMENT)" paths "$(PROFILE)"

verify:
	@"$(DEVELOPMENT)" verify "$(PROFILE)"

shell:
	@"$(DEVELOPMENT)" shell

image:
	@"$(DEVELOPMENT)" image

endif
