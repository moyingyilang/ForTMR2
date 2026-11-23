CC      ?= clang
AR      ?= ar
CFLAGS  ?= -O2 -std=c11 -Wall -Wextra -Iinclude
BUILD   := build
LIB     := libefml4a.a
OBJ     := $(BUILD)/sources.o

all: $(LIB)
$(LIB): $(OBJ)
	$(AR) rcs $@ $^
$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD):
	@mkdir -p $(BUILD)
clean:
	rm -rf $(BUILD) $(LIB)
.PHONY: all clean
