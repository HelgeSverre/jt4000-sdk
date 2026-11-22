# JT-4000 custom firmware SDK. Apps are the folders in sdk/examples.
# Needs: arm-none-eabi-gcc, uv; ffmpeg for videos; the `speak` command to change word banks.

uv := "uv"
panel := "uv run --with pygame-ce --with sounddevice --with numpy tools/panel.py"
stock_image := "firmware/JT-4000_INTFLASH_CRY_V115.bin"

# Show available recipes
default:
    @just --list

# === Firmware ===

# List the apps that can be built
[group('firmware')]
apps:
    @ls sdk/examples

# Build an app for the synth (e.g. just build groove)
[group('firmware')]
build app="hello":
    make -C sdk APP={{ app }}

# Build an app and flash it to the synth (e.g. just flash groove)
[group('firmware')]
flash app:
    make -C sdk APP={{ app }} flash

# Start a new app from examples/hello
[group('firmware')]
new app:
    make -C sdk APP={{ app }} new

# Flash the factory firmware back (downloads it first if needed)
[group('firmware')]
stock:
    test -f {{ stock_image }} || {{ uv }} run tools/fwfetch.py
    {{ uv }} run tools/flash.py flash {{ stock_image }}

# Download the factory firmware images
[group('firmware')]
fetch *args:
    {{ uv }} run tools/fwfetch.py {{ args }}

# Remove everything that was built
[group('firmware')]
clean:
    make -C sdk clean

# === Preview ===

# Run an app in a window that looks like the synth, with sound (e.g. just host groove)
[group('preview')]
host app *args:
    {{ panel }} {{ app }} {{ args }}

# Save a picture of an app after some milliseconds (e.g. just shot drone 3000)
[group('preview')]
shot app ms="3000" *args:
    {{ panel }} {{ app }} --ms {{ ms }} {{ args }}

# Save a video of an app with sound (e.g. just video choir 8000 choir.mp4)
[group('preview')]
video app ms="8000" out="demo.mp4" *args:
    {{ panel }} {{ app }} --ms {{ ms }} --video {{ out }} {{ args }}

# Run the real ARM binary in the slow, exact simulator and print the display
[group('preview')]
sim app="hello" *args:
    make -C sdk APP={{ app }}
    {{ uv }} run --with unicorn tools/emu.py sdk/build/{{ app }}/{{ app }}.bin {{ args }}

# === Words ===

# Rebuild an app's words.h from its words.txt (e.g. just words choir --rate 110)
[group('words')]
words app *args:
    {{ uv }} run --with numpy tools/lpcwords.py --list sdk/examples/{{ app }}/words.txt --out sdk/examples/{{ app }}/words.h {{ args }}

# Turn MIDI files into riffs for the trance app
[group('words')]
riffs *args:
    {{ uv }} run --with mido tools/riff.py {{ args }}

# === QA ===

# Build every app for the synth and for the computer
[group('qa')]
build-all:
    for app in $(ls sdk/examples); do make -s -C sdk APP=$app && make -s -C sdk APP=$app host || exit 1; done

# Check lint and formatting without changing files
[group('qa')]
lint:
    {{ uv }} run ruff check .
    {{ uv }} run ruff format --check .

# Format all Python files
[group('qa')]
format:
    {{ uv }} run ruff format .
    {{ uv }} run ruff check --fix .

# Pre-commit gate: lint, then build everything
[group('qa')]
check: lint build-all
