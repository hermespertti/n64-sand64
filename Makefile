ROMNAME := sand64
BUILD_DIR := build

export N64_INST ?= $(HOME)/n64-sdk
# cross-gcc from pacman, not under SDK prefix
export N64_GCCPREFIX = /usr

all: $(ROMNAME).z64

include $(N64_INST)/include/n64.mk

C_FILES := main.c
ASM_FILES := rsp_sand64.S
OBJS := $(addprefix $(BUILD_DIR)/,$(C_FILES:.c=.o)) $(addprefix $(BUILD_DIR)/,$(ASM_FILES:.S=.o))

CFLAGS += $(EXTRA_CFLAGS)
ifeq ($(USE_RSP),1)
CFLAGS += -DUSE_RSP
endif

$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)

$(ROMNAME).z64: N64_ROM_TITLE = "SAND64"

clean:
	$(RM) -r $(BUILD_DIR) *.z64
.PHONY: all clean
