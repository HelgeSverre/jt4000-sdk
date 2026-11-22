# JT-4000 custom firmware SDK. Apps are the folders in sdk/examples.
# Needs: arm-none-eabi-gcc, uv; ffmpeg for videos; the `speak` command to change word banks.

uv := "uv"
panel := "uv run --with pygame-ce --with sounddevice --with numpy tools/panel.py"
stock_image := "firmware/JT-4000_INTFLASH_CRY_V115.bin"

# List recipes
default:
    @just --list

# === Firmware ===

# List apps
[group('firmware')]
apps:
    @ls sdk/examples

# Build an app
[group('firmware')]
build app="hello":
    make -C sdk APP={{ app }}

# Build and flash an app
[group('firmware')]
flash app:
    make -C sdk APP={{ app }} flash

# New app from hello
[group('firmware')]
new app:
    make -C sdk APP={{ app }} new

# Flash factory firmware
[group('firmware')]
stock:
    test -f {{ stock_image }} || {{ uv }} run tools/fwfetch.py
    {{ uv }} run tools/flash.py flash {{ stock_image }}

# Download factory firmware
[group('firmware')]
fetch *args:
    {{ uv }} run tools/fwfetch.py {{ args }}

# Remove build output
[group('firmware')]
clean:
    make -C sdk clean

# === Preview ===

# Run an app in the synth window
[group('preview')]
host app *args:
    {{ panel }} {{ app }} {{ args }}

# Screenshot an app
[group('preview')]
shot app ms="3000" *args:
    {{ panel }} {{ app }} --ms {{ ms }} {{ args }}

# Record a video of an app
[group('preview')]
video app ms="8000" out="demo.mp4" *args:
    {{ panel }} {{ app }} --ms {{ ms }} --video {{ out }} {{ args }}

# Redraw the README header
[group('preview')]
header:
    {{ uv }} run --with pillow --with pygame-ce --with numpy tools/header_gif.py

# Run the ARM binary in the simulator
[group('preview')]
sim app="hello" *args:
    make -C sdk APP={{ app }}
    {{ uv }} run --with unicorn tools/emu.py sdk/build/{{ app }}/{{ app }}.bin {{ args }}

# === Words ===

# Rebuild an app's word bank
[group('words')]
words app *args:
    {{ uv }} run --with numpy tools/lpcwords.py --list sdk/examples/{{ app }}/words.txt --out sdk/examples/{{ app }}/words.h {{ args }}

# MIDI files to trance riffs
[group('words')]
riffs *args:
    {{ uv }} run --with mido tools/riff.py {{ args }}

# === QA ===

# Build every app, both targets
[group('qa')]
build-all:
    for app in $(ls sdk/examples); do make -s -C sdk APP=$app && make -s -C sdk APP=$app host || exit 1; done

# Lint, no changes
[group('qa')]
lint:
    {{ uv }} run ruff check .
    {{ uv }} run ruff format --check .

# Format Python
[group('qa')]
format:
    {{ uv }} run ruff format .
    {{ uv }} run ruff check --fix .

# Lint and build all
[group('qa')]
check: lint build-all
