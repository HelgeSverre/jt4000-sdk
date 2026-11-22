# JT-4000 SDK. Usage:
#   make                 build examples/hello
#   make APP=myapp       build examples/myapp
#   make flash           build, package and flash (synth on USB; or hold OSC+SETTINGS at power-on)
#   make sim             run the build in the simulator and draw the display
#   make host            build it for this computer, for tools/panel.py (a window that looks like the synth)
#   make new APP=myapp   start examples/myapp from examples/hello
APP    ?= hello
DEFS   ?=
TAG    := $(APP)$(subst =,,$(subst -D,_,$(strip $(DEFS))))
BUILD  := build/$(TAG)
CC     := arm-none-eabi-gcc
CFLAGS := -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16 -O2 -g -ffreestanding -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-override-init -Iinclude $(DEFS)
LDFLAGS := -nostdlib -Wl,--gc-sections -Wl,-T,ld/jt4000.ld -Wl,-Map,$(BUILD)/$(APP).map
SRC    := $(wildcard src/*.c) $(wildcard examples/$(APP)/*.c)
TOOLS  := ../tools

all: $(BUILD)/$(APP).cry.bin

$(BUILD)/$(APP).elf: $(SRC) include/*.h $(wildcard examples/$(APP)/*.h) ld/jt4000.ld
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SRC) $(LDFLAGS) -lgcc -o $@
	arm-none-eabi-size $@

$(BUILD)/$(APP).bin: $(BUILD)/$(APP).elf
	arm-none-eabi-objcopy -O binary $< $@

# Pad, write the trailer the bootloader checks, and encrypt.
$(BUILD)/$(APP).cry.bin: $(BUILD)/$(APP).bin
	cd .. && uv run tools/fwpack.py build sdk/$< sdk/$@

flash: $(BUILD)/$(APP).cry.bin
	cd .. && uv run tools/flash.py flash sdk/$<

sim: $(BUILD)/$(APP).bin
	cd .. && uv run --with unicorn tools/emu.py sdk/$<

# The same source, built for this computer: a shared library that tools/panel.py draws
# and plays. host/host.c stands in for the hardware drivers.
HOSTLIB := build/host/$(APP).$(if $(filter Darwin,$(shell uname)),dylib,so)
host: $(HOSTLIB)
$(HOSTLIB): src/dsp.c src/lpc.c src/menu.c src/oled.c host/host.c $(wildcard examples/$(APP)/*.c) include/*.h $(wildcard examples/$(APP)/*.h)
	@mkdir -p build/host
	cc -O2 -g -shared -fPIC -DJT_HOST -Dmain=jt_firmware_main -Wall -Wextra -Wno-override-init -Wno-initializer-overrides \
	   -Iinclude src/dsp.c src/lpc.c src/menu.c src/oled.c host/host.c $(wildcard examples/$(APP)/*.c) -lpthread -o $@

new:
	@test ! -e examples/$(APP) || { echo "examples/$(APP) already exists"; exit 1; }
	cp -R examples/hello examples/$(APP)
	@echo "created examples/$(APP); build it with: make APP=$(APP)"

clean:
	rm -rf build

.PHONY: all flash sim new clean host
