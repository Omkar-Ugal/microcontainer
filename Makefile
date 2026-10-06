CC ?= gcc
BUILD_DIR ?= build
SAN_CFLAGS := -O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow \
              -Wconversion -Wstrict-prototypes -Wmissing-prototypes \
              -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS += -D_GNU_SOURCE -D_XOPEN_SOURCE=700 -Iinclude
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow \
          -Wconversion -Wstrict-prototypes -Wmissing-prototypes -fstack-protector-strong
LDFLAGS ?=

SOURCES := src/main.c src/config.c src/logging.c src/filesystem.c src/namespace.c \
           src/network.c src/cgroup.c src/security.c src/lifecycle.c
OBJECTS := $(SOURCES:src/%.c=$(BUILD_DIR)/%.o)
TARGET := $(BUILD_DIR)/microcontainer

.PHONY: all clean test integration debug sanitize

all: $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(LDFLAGS) $^ -o $@

$(BUILD_DIR)/%.o: src/%.c include/microcontainer.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/test_config: tests/test_config.c src/config.c src/logging.c src/filesystem.c include/microcontainer.h
	@mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_config.c src/config.c src/logging.c src/filesystem.c -o $@

test: $(TARGET) $(BUILD_DIR)/test_config
	$(BUILD_DIR)/test_config
	bash tests/test_cli.sh $(TARGET)

integration: $(TARGET)
	bash tests/integration.sh $(TARGET)

debug:
	$(MAKE) BUILD_DIR=build/debug CFLAGS="-O0 -g3 -std=c11 -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wshadow -Wconversion -Wstrict-prototypes -Wmissing-prototypes -fstack-protector-strong" all

sanitize:
	$(MAKE) BUILD_DIR=build/sanitize CFLAGS="$(SAN_CFLAGS)" LDFLAGS="-fsanitize=address,undefined" all
	$(MAKE) BUILD_DIR=build/sanitize CFLAGS="$(SAN_CFLAGS)" LDFLAGS="-fsanitize=address,undefined" test-sanitized

.PHONY: test-sanitized
test-sanitized: $(BUILD_DIR)/microcontainer $(BUILD_DIR)/test_config
	$(BUILD_DIR)/test_config
	bash tests/test_cli.sh $(BUILD_DIR)/microcontainer

clean:
	rm -rf build

-include $(OBJECTS:.o=.d)
