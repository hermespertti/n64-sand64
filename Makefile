ROMNAME := sand64
BUILD_DIR := build

export N64_INST ?= $(HOME)/n64-sdk
# cross-gcc from pacman, not under SDK prefix
export N64_GCCPREFIX = /usr

all: $(ROMNAME).z64

include $(N64_INST)/include/n64.mk

C_FILES := main.c
# rsp_sand64.S lands in stage 2 (RSP physics port)
OBJS := $(addprefix $(BUILD_DIR)/,$(C_FILES:.c=.o))

CFLAGS += $(EXTRA_CFLAGS)

$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)

$(ROMNAME).z64: N64_ROM_TITLE = "SAND64"

clean:
	$(RM) -r $(BUILD_DIR) *.z64
.PHONY: all clean
