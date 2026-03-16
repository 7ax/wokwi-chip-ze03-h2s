SOURCES = src/main.c
TARGET  = dist/chip.wasm
CFLAGS  = -Wall -Wextra -Werror

.PHONY: all clean

all: $(TARGET) dist/chip.json

clean:
	rm -rf dist

dist:
	mkdir -p dist

$(TARGET): dist $(SOURCES) src/wokwi-api.h
	clang --target=wasm32-unknown-wasi --sysroot /opt/wasi-libc \
		-nostartfiles -Wl,--import-memory -Wl,--export-table \
		-Wl,--no-entry $(CFLAGS) -o $(TARGET) $(SOURCES)

dist/chip.json: dist chip.json
	cp chip.json dist
