UNAME_S := $(shell uname -s)
CC ?= cc
GO ?= go
GOCACHE := $(CURDIR)/.gocache

BUILD_DIR := build
BUILD_TARGET := all-build
SRC_DIR := src
INCLUDE_DIR := include
MODULE_NAME := valkey-ftdc
MODULE_BASENAME := $(MODULE_NAME).so
MODULE_OUTPUT := $(BUILD_DIR)/$(MODULE_BASENAME)
CLI_OUTPUT := $(BUILD_DIR)/valkey-ftdcstat

CFLAGS_COMMON := -std=c11 -Wall -Wextra -Werror -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -I$(INCLUDE_DIR)

ifeq ($(UNAME_S),Darwin)
SHARED_LDFLAGS := -dynamiclib -undefined dynamic_lookup
HOST_SRC := $(SRC_DIR)/hoststats_stub.c
else
SHARED_LDFLAGS := -shared
HOST_SRC := $(SRC_DIR)/hoststats_linux.c
endif

MODULE_SRCS := \
	$(SRC_DIR)/ftdc.c \
	$(SRC_DIR)/collector.c \
	$(SRC_DIR)/writer.c \
	$(SRC_DIR)/rotation.c \
	$(SRC_DIR)/redact.c \
	$(HOST_SRC)

.PHONY: all $(BUILD_TARGET) module cli test integration clean

all: $(BUILD_TARGET)

$(BUILD_TARGET): module cli

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

module: $(MODULE_OUTPUT)

$(MODULE_OUTPUT): $(MODULE_SRCS) $(SRC_DIR)/ftdc.h $(SRC_DIR)/collector.h $(SRC_DIR)/writer.h $(SRC_DIR)/rotation.h $(SRC_DIR)/hoststats.h $(SRC_DIR)/redact.h | $(BUILD_DIR)
	$(CC) $(CFLAGS_COMMON) -fPIC $(SHARED_LDFLAGS) -o $@ $(MODULE_SRCS)

cli: $(CLI_OUTPUT)

$(CLI_OUTPUT): go.mod tools/valkey-ftdcstat/main.go | $(BUILD_DIR)
	GOCACHE=$(GOCACHE) $(GO) build -o $@ ./tools/valkey-ftdcstat

test: $(BUILD_TARGET)
	GOCACHE=$(GOCACHE) $(GO) test ./...

integration: $(BUILD_TARGET)
	GOCACHE=$(GOCACHE) $(GO) test -tags=integration ./tests/integration

clean:
	rm -rf $(BUILD_DIR)
