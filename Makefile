CC := gcc
CFLAGS := -Wall -lSDL2 -lSDL2_ttf -lm -lavformat -lavcodec -lswscale -lavutil -lz
DEBUGFLAGS := -g -O0
RELEASEFLAGS := -O3
APP_NAME := av

SRC_DIR := src
BUILD_DIR := build

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)

all: $(OBJS)
	$(CC) $(DEBUGFLAGS) $(CFLAGS) $(OBJS) -o $(BUILD_DIR)/$(APP_NAME)

$(BUILD_DIR)/%.o : %.c
	mkdir -p $(BUILD_DIR)
	$(CC) $(DEBUGFLAGS) $(CFLAGS) -c $< -o $@

clean:
	rm -r $(BUILD_DIR)
