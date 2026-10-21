# Arm GNU toolchain (arm-none-eabi-gcc); the released patch is built with GCC 16.1.0.
ARM_PREFIX ?= arm-none-eabi-
PATCH := build/assimil8or.a8patch
SOURCES := $(wildcard src/*.c src/*.h src/*.S src/*.ld ext/*.c ext/*.h) src/patch_layout.json
.PHONY: all check release clean
all: $(PATCH)
$(PATCH): $(SOURCES) VERSION tools/make_patch.py tools/eq_coefficients.py patcher.py
	python3 tools/make_patch.py $@ --prefix '$(ARM_PREFIX)'
# With A8_STOCK_APP set to Rossum's v3.04 download, the patch is applied to it as well.
check: $(PATCH)
	python3 tools/check_patch.py $(PATCH) $(A8_STOCK_APP)
release: $(PATCH)
	@test -n "$(A8_STOCK_APP)" || { echo "Set A8_STOCK_APP to Rossum's v3.04 download" >&2; exit 1; }
	python3 tools/package_release.py "$(A8_STOCK_APP)"
clean:
	rm -rf build
