CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lm -lwayland-client -lcairo -levdev -ludev -pthread

SRC_DIR = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/trail.c $(SRC_DIR)/input.c $(SRC_DIR)/wlr-layer-shell-client-protocol.c $(SRC_DIR)/xdg-shell-client-protocol.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET = mouse-trail

WAYLAND_CFLAGS = $(shell pkg-config --cflags wayland-client 2>/dev/null || echo "")
CAIRO_CFLAGS = $(shell pkg-config --cflags cairo 2>/dev/null || echo "")
EVDEV_CFLAGS = $(shell pkg-config --cflags libevdev 2>/dev/null || echo "")
UDEV_CFLAGS = $(shell pkg-config --cflags libudev 2>/dev/null || echo "")
INCLUDES = -I$(SRC_DIR) $(WAYLAND_CFLAGS) $(CAIRO_CFLAGS) $(EVDEV_CFLAGS) $(UDEV_CFLAGS)

.PHONY: all clean test input-probe

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

-include $(OBJS:.o=.d)

test: | $(BUILD_DIR)
	$(CC) $(CFLAGS) -DTRAIL_TEST -I$(SRC_DIR) $(SRC_DIR)/trail.c -o $(BUILD_DIR)/trail_test -lm
	$(BUILD_DIR)/trail_test
	$(CC) $(CFLAGS) -DINPUT_TEST $(INCLUDES) $(SRC_DIR)/input.c -o $(BUILD_DIR)/input_test -levdev -ludev -pthread
	$(BUILD_DIR)/input_test

# Read-only live-input probe; not part of sandboxed unit tests.
input-probe: | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) tests/input-probe.c $(SRC_DIR)/input.c -o $(BUILD_DIR)/input_probe -levdev -ludev -pthread
	$(BUILD_DIR)/input_probe

clean:
	remove-without-permission -rf $(BUILD_DIR) $(TARGET)
