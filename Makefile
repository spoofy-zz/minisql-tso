ifneq (,$(wildcard .env))
include .env
endif

ifeq ($(strip $(MINISQL_TOOLCHAIN_BIN)),)
ifneq (,$(wildcard $(HOME)/.local/bin/cc370))
MINISQL_TOOLCHAIN_BIN := $(HOME)/.local/bin
endif
endif

ifneq ($(strip $(MINISQL_TOOLCHAIN_BIN)),)
export PATH := $(MINISQL_TOOLCHAIN_BIN):$(PATH)
CC := $(MINISQL_TOOLCHAIN_BIN)/cc370
AS := $(MINISQL_TOOLCHAIN_BIN)/as370
LD := $(MINISQL_TOOLCHAIN_BIN)/ld370
AR := $(MINISQL_TOOLCHAIN_BIN)/ar370
endif

MBT_ROOT := mbt
include $(MBT_ROOT)/mk/mbt.mk

.PHONY: deploy-mvs deploy-mvs-dry-run

deploy-mvs:
	@tools/deploy-mvs.sh

deploy-mvs-dry-run:
	@tools/deploy-mvs.sh --dry-run

HOST_CC ?= cc
HOST_CFLAGS ?= -std=c99 -Wall -Wextra -O2 -I.mbt
HOST_ENGINE_SOURCES := $(filter-out src/minisql.c src/msqltso.c,$(wildcard src/*.c))

.PHONY: check-host
check-host: build/host/minisql build/host/msqltso build/host/terminal3270
	python3 tests/select_alignment.py build/host/minisql
	python3 tests/regression.py build/host/minisql
	build/host/terminal3270

build/host/minisql: src/minisql.c $(HOST_ENGINE_SOURCES) include/minisql.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude src/minisql.c $(HOST_ENGINE_SOURCES) -o $@

build/host/msqltso: src/msqltso.c $(HOST_ENGINE_SOURCES) include/minisql.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude src/msqltso.c $(HOST_ENGINE_SOURCES) -o $@

build/host/terminal3270: tests/terminal3270.c include/terminal3270.h
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude $< -o $@
