SRCDIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
COREDIR := $(SRCDIR)/../core

# Build directory: auto-named build-<arch>, override with BUILDDIR=xxx
TOOLCHAIN_ARCH = $(shell LANG=C $(CC) -v 2>&1 | awk '/Target:(.*)/{ print $$2 }' | cut -d- -f1)
TRANSLATION_ENABLED ?= TRUE
SUPPORT_LINUX ?= TRUE

ifeq "$(TOOLCHAIN_ARCH)" ""
    $(error Could not determine the toolchain architecture, please specify manually)
endif

BUILDDIR ?= $(SRCDIR)/build-$(TOOLCHAIN_ARCH)

# ---- Internal flags (always applied, not overridable) ----

INT_FLAGS := -g -O3 -DBENCHMARK -Wall -I$(SRCDIR)/.. -I$(SRCDIR)/vendor

# ---- Architecture-specific translation sources ----

CORE_ASMSOURCES :=
CORE_CSOURCES :=
CORE_CPPSOURCES :=

ifeq "$(TRANSLATION_ENABLED)" "TRUE"
    ifneq "$(filter i%86,$(TOOLCHAIN_ARCH))" ""
        CORE_ASMSOURCES += asmcode_x86.S
        CORE_CSOURCES += translation_x86.c
    else ifeq "$(TOOLCHAIN_ARCH)" "x86_64"
        CORE_ASMSOURCES += asmcode_x86_64.S
        CORE_CSOURCES += asmcode.c translate_x86_64.c
    else ifneq "$(filter arm%,$(TOOLCHAIN_ARCH))" ""
        CORE_ASMSOURCES += asmcode_arm.S
        CORE_CSOURCES += asmcode.c
        CORE_CPPSOURCES += translate_arm.cpp
    else ifeq "$(TOOLCHAIN_ARCH)" "aarch64"
        CORE_ASMSOURCES += asmcode_aarch64.S
        CORE_CSOURCES += asmcode.c
        CORE_CPPSOURCES += translate_aarch64.cpp
    else
        $(error Unknown architecture $(TOOLCHAIN_ARCH))
    endif
else
    CORE_CSOURCES += asmcode.c
    INT_FLAGS += -DNO_TRANSLATION
endif

ifeq "$(SUPPORT_LINUX)" "TRUE"
    INT_FLAGS += -DSUPPORT_LINUX
endif

# User-provided CFLAGS/CXXFLAGS/LFLAGS are appended (not overridden)
ALL_CFLAGS   := -std=c11 $(INT_FLAGS) $(CFLAGS)
ALL_CXXFLAGS := -std=c++11 $(INT_FLAGS) $(CXXFLAGS)
ALL_LFLAGS   := $(LFLAGS)
LIBS := -lz

# ---- Source lists (basenames, relative to their respective dirs) ----

CORE_CSOURCES += armsnippets_loader.c casplus.c des.c disasm.c gdbstub.c \
                 interrupt.c lcd.c link.c mem.c misc.c \
                 mmu.c schedule.c serial.c sha256.c usb.c usblink.c

CORE_CPPSOURCES += arm_interpreter.cpp coproc.cpp cpu.cpp debug.cpp emu.cpp \
                   flash.cpp gif.cpp thumb_interpreter.cpp usblink_queue.cpp \
                   keypad.cpp cx2.cpp usb_cx2.cpp usblink_cx2.cpp fieldparser.cpp

# os subdirectory under core
CORE_OS_CSOURCES := os-linux.c

# Local sources (relative to SRCDIR)
LOCAL_CPPSOURCES := main.cpp mcp_headless.cpp

# Vendor sources (relative to SRCDIR)
VENDOR_CSOURCES := vendor/cJSON.c

# ---- Object file paths (all under BUILDDIR) ----

OBJS  = $(patsubst %.S, $(BUILDDIR)/core/%.o, $(CORE_ASMSOURCES))
OBJS += $(patsubst %.c, $(BUILDDIR)/core/%.o, $(CORE_CSOURCES))
OBJS += $(patsubst %.cpp, $(BUILDDIR)/core/%.o, $(CORE_CPPSOURCES))
OBJS += $(patsubst %.c, $(BUILDDIR)/core/os/%.o, $(CORE_OS_CSOURCES))
OBJS += $(patsubst %.cpp, $(BUILDDIR)/%.o, $(LOCAL_CPPSOURCES))
OBJS += $(patsubst %.c, $(BUILDDIR)/%.o, $(VENDOR_CSOURCES))

OUTPUT_BIN := $(BUILDDIR)/firebird-headless

all: $(OUTPUT_BIN)

# ---- Compile rules ----

$(BUILDDIR)/core/%.o: $(COREDIR)/%.S Makefile
	@mkdir -p $(dir $@)
	$(CC) -c $(ALL_CFLAGS) $< -o $@

$(BUILDDIR)/core/%.o: $(COREDIR)/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -c $(ALL_CFLAGS) $< -o $@

$(BUILDDIR)/core/%.o: $(COREDIR)/%.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) -c $(ALL_CXXFLAGS) $< -o $@

$(BUILDDIR)/core/os/%.o: $(COREDIR)/os/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -c $(ALL_CFLAGS) $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.c Makefile
	@mkdir -p $(dir $@)
	$(CC) -c $(ALL_CFLAGS) $< -o $@

$(BUILDDIR)/%.o: $(SRCDIR)/%.cpp Makefile
	@mkdir -p $(dir $@)
	$(CXX) -c $(ALL_CXXFLAGS) $< -o $@

# ---- Link ----

$(OUTPUT_BIN): $(OBJS)
	$(CXX) $(ALL_CXXFLAGS) $(ALL_LFLAGS) $^ $(LIBS) -o $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean
