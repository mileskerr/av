CC := gcc
DEBUGFLAGS := -g -O0 
#-fsanitize=address
RELEASEFLAGS := -O3
CFLAGS := -Wall -Wextra $(DEBUGFLAGS)
LDFLAGS := -lSDL2 -lSDL2_ttf -lm -lavformat -lavcodec -lswscale -lavutil -lswresample -lz
APP_NAME := av

SRC_DIR := src
OBJ_DIR := obj
BUILD_DIR := build

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)

all: $(OBJS)
	mkdir -p $(BUILD_DIR)
	$(CC) $(CFLAGS) $(OBJS) -o $(BUILD_DIR)/$(APP_NAME) $(LDFLAGS)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(info @a)
	mkdir -p $(OBJ_DIR)
	mkdir -p '$(@D)'
	$(CC) $(CFLAGS) -c $< -o $@



clean:
	rm -r $(BUILD_DIR)
	rm -r $(OBJ_DIR)
