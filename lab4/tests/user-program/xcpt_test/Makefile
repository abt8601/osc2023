DEFAULT_PROFILE = DEBUG
SRC_DIR         = src
BUILD_DIR       = build

AS      = aarch64-linux-gnu-as
CC      = aarch64-linux-gnu-gcc
CPP     = aarch64-linux-gnu-cpp
OBJCOPY = aarch64-linux-gnu-objcopy

CPPFLAGS_BASE = -Iinclude

ASFLAGS_DEBUG = -g

CFLAGS_BASE    = -std=c17 -pedantic-errors -Wall -Wextra -ffreestanding \
                 -mcpu=cortex-a53 -mno-outline-atomics -mstrict-align
CFLAGS_DEBUG   = -g
CFLAGS_RELEASE = -O3 -flto

LDFLAGS_BASE    = -nostdlib -Xlinker --build-id=none
LDFLAGS_RELEASE = -flto -Xlinker --gc-sections

LDLIBS_BASE = -lgcc

OBJS      = start
LD_SCRIPT = $(SRC_DIR)/linker.ld

# ------------------------------------------------------------------------------

PROFILE = $(DEFAULT_PROFILE)
OUT_DIR = $(BUILD_DIR)/$(PROFILE)

CPPFLAGS = $(CPPFLAGS_BASE) $(CPPFLAGS_$(PROFILE))
ASFLAGS  = $(ASFLAGS_BASE) $(ASFLAGS_$(PROFILE))
CFLAGS   = $(CFLAGS_BASE) $(CFLAGS_$(PROFILE))
LDFLAGS  = $(LDFLAGS_BASE) $(LDFLAGS_$(PROFILE))
LDLIBS   = $(LDLIBS_BASE) $(LDLIBS_$(PROFILE))

OBJ_PATHS = $(addprefix $(OUT_DIR)/,$(addsuffix .o,$(OBJS)))

# ------------------------------------------------------------------------------

.PHONY: all clean-profile clean

all: $(OUT_DIR)/xcpt_test.img

$(OUT_DIR)/xcpt_test.img: $(OUT_DIR)/xcpt_test.elf
	@mkdir -p $(@D)
	$(OBJCOPY) -O binary $^ $@

$(OUT_DIR)/xcpt_test.elf: $(OBJ_PATHS) $(LD_SCRIPT)
	@mkdir -p $(@D)
	$(CC) -T $(LD_SCRIPT) $(LDFLAGS) $(filter-out $(LD_SCRIPT),$^) $(LDLIBS) \
		-o $@

$(OUT_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -MMD -MP -MF $(OUT_DIR)/$*.d $< -o $@

$(OUT_DIR)/%.o: $(OUT_DIR)/%.s
	@mkdir -p $(@D)
	$(AS) $(ASFLAGS) $^ -o $@

$(OUT_DIR)/%.s: $(SRC_DIR)/%.S
	@mkdir -p $(@D)
	$(CPP) $(CPPFLAGS) $^ -o $@

clean-profile:
	$(RM) -r $(OUT_DIR)

clean:
	$(RM) -r $(BUILD_DIR)

-include $(OUT_DIR)/*.d
