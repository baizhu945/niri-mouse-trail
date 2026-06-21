CC = gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lm -lwayland-client -lcairo -levdev

SRC_DIR = src
BUILD_DIR = build

SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/trail.c $(SRC_DIR)/wlr-layer-shell-client-protocol.c $(SRC_DIR)/xdg-shell-client-protocol.c
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))
TARGET = mouse-trail

WAYLAND_CFLAGS = $(shell pkg-config --cflags wayland-client 2>/dev/null || echo "")
CAIRO_CFLAGS = $(shell pkg-config --cflags cairo 2>/dev/null || echo "")
EVDEV_CFLAGS = $(shell pkg-config --cflags libevdev 2>/dev/null || echo "")
INCLUDES = -I$(SRC_DIR) $(WAYLAND_CFLAGS) $(CAIRO_CFLAGS) $(EVDEV_CFLAGS)

.PHONY: all clean test

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test:
	gcc -DTRAIL_TEST -I$(SRC_DIR) $(SRC_DIR)/trail.c -o /tmp/trail_test -lm
	/tmp/trail_test

clean:
	rm -rf $(BUILD_DIR) $(TARGET)
