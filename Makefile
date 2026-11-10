# Convenience wrapper around the CMake build.
BUILD ?= build
PREFIX ?= /usr/local

UPSTREAM ?= /tmp/arrpc-ref

.PHONY: all build test asan compare clean install

all: build

build:
	cmake -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: build
	python3 tools/test_server.py $(BUILD)/arrpcf

asan:
	cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug \
		-DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g" \
		-DCMAKE_C_FLAGS="-fsanitize=address,undefined -g" \
		-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
	cmake --build build-asan -j
	python3 tools/test_server.py build-asan/arrpcf

compare: build
	python3 tools/compare_upstream.py $(UPSTREAM) $(BUILD)/arrpcf

install: build
	cmake --install $(BUILD) --prefix $(PREFIX)

clean:
	rm -rf build build-asan
