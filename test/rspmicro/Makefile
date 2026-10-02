ROMNAME := micro
BUILD_DIR := build

export N64_INST ?= $(HOME)/n64-sdk
export N64_GCCPREFIX = /usr

all: $(ROMNAME).z64

include $(N64_INST)/include/n64.mk

C_FILES := main.c
ASM_FILES := rsp_micro.S
OBJS := $(addprefix $(BUILD_DIR)/,$(C_FILES:.c=.o)) $(addprefix $(BUILD_DIR)/,$(ASM_FILES:.S=.o))

CFLAGS += $(EXTRA_CFLAGS)

$(BUILD_DIR)/$(ROMNAME).elf: $(OBJS)

$(ROMNAME).z64: N64_ROM_TITLE = "RSPMICRO"

clean:
	$(RM) -r $(BUILD_DIR) *.z64
.PHONY: all clean
