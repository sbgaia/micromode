.PHONY: all lib test unit-test asan format format-check clean

# Extra flags forwarded to every cmake configure (e.g. -DREACTOR_UC_PATH=...)
CMAKE_EXTRA_FLAGS ?=

SRC_FILES := $(wildcard src/*.c) $(wildcard test/unit/*.c)
HDR_FILES := $(wildcard include/*.h) $(wildcard test/unit/*.h)

all: lib

# Build micromode as a static library
lib:
	cmake -Bbuild $(CMAKE_EXTRA_FLAGS)
	cmake --build build

# Build and run the unit tests
test: unit-test

unit-test:
	@test -n "$(REACTOR_UC_PATH)" || { echo "REACTOR_UC_PATH is not set"; exit 1; }
	cmake -Bbuild -DMICROMODE_BUILD_TESTS=ON $(CMAKE_EXTRA_FLAGS)
	cmake --build build
	cd build && ctest --output-on-failure

# Build and run the unit tests under AddressSanitizer
asan:
	cmake -Bbuild -DASAN=ON -DMICROMODE_BUILD_TESTS=ON $(CMAKE_EXTRA_FLAGS)
	cmake --build build
	cd build && ctest --output-on-failure

format:
	clang-format -i -style=file $(SRC_FILES) $(HDR_FILES)

format-check:
	clang-format --dry-run --Werror -style=file $(SRC_FILES) $(HDR_FILES) || { echo "Run 'make format' to fix formatting issues"; exit 1; }

clean:
	rm -rf build
