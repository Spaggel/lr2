# macOS build. Needs: xcode-select --install; brew install cmake ninja sdl3
#   make        configure (first time) and build build/native/lr2
#   make run    build, then start the game
#   make clean  remove the build directory
#   make package  player package dist/LR2-macos.zip (tools/package.sh)
BUILD := build/native

all: $(BUILD)/build.ninja
	ninja -C $(BUILD)

$(BUILD)/build.ninja:
	git submodule update --init third_party/zlib
	cmake -S . -B $(BUILD) -G Ninja -DCMAKE_PREFIX_PATH="$$(brew --prefix)"

run: all
	$(BUILD)/lr2

clean:
	rm -rf $(BUILD)

package:
	tools/package.sh

.PHONY: all run clean package
