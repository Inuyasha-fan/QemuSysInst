CC := gcc

PROJECT_DIR := .
SRC_DIR := $(PROJECT_DIR)/src
BUILD_DIR := $(PROJECT_DIR)/build

TEST_QEMU_CLIENT_TARGET := $(BUILD_DIR)/test_qemu_client
TEST_PEACH_CLIENT_TARGET := $(BUILD_DIR)/test_peach_client

PKG_CONFIG ?= pkg-config
GLIB_PKG := glib-2.0
PKG_MODULES := $(GLIB_PKG)

CPPFLAGS += $(shell $(PKG_CONFIG) --cflags $(PKG_MODULES))
CPPFLAGS += -I$(SRC_DIR)/socket -I$(SRC_DIR)/test -I$(SRC_DIR)/coverage
LDLIBS += $(shell $(PKG_CONFIG) --libs $(PKG_MODULES))

CFLAGS ?= -Wall -Wextra -Wpedantic -O2

.PHONY: all test_qemu_client test_peach_client clean

all: test_qemu_client test_peach_client

test_qemu_client: $(TEST_QEMU_CLIENT_TARGET)

test_peach_client: $(TEST_PEACH_CLIENT_TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_QEMU_CLIENT_TARGET): $(SRC_DIR)/test/test_qemu_client.c $(SRC_DIR)/socket/socket_comm.c $(SRC_DIR)/socket/socket_comm.h $(SRC_DIR)/coverage/coverage_comm.c $(SRC_DIR)/coverage/coverage_comm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC_DIR)/test/test_qemu_client.c $(SRC_DIR)/socket/socket_comm.c $(SRC_DIR)/coverage/coverage_comm.c $(LDLIBS)

$(TEST_PEACH_CLIENT_TARGET): $(SRC_DIR)/test/test_peach_client.c $(SRC_DIR)/socket/socket_comm.c $(SRC_DIR)/socket/socket_comm.h $(SRC_DIR)/coverage/coverage_comm.c $(SRC_DIR)/coverage/coverage_comm.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRC_DIR)/test/test_peach_client.c $(SRC_DIR)/socket/socket_comm.c $(SRC_DIR)/coverage/coverage_comm.c $(LDLIBS)

clean:
	rm -f $(TEST_QEMU_CLIENT_TARGET) $(TEST_PEACH_CLIENT_TARGET) $(BUILD_DIR)/test_client $(BUILD_DIR)/peach_client $(BUILD_DIR)/test_server
