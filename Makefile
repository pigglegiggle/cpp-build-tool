CXX := clang++
CPPFLAGS := -Iinclude
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
CORE := src/hash.cpp src/project.cpp src/process.cpp src/build.cpp
HEADERS := $(wildcard include/cppbuild/*.hpp)
.PHONY: all test sanitize demo benchmark
all: build/cppbuild
build/cppbuild: src/main.cpp $(CORE) $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/main.cpp $(CORE) -o $@
build/unit-tests: tests/unit.cpp $(CORE) $(HEADERS)
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/unit.cpp $(CORE) -o $@
test: build/cppbuild build/unit-tests
	./build/unit-tests
	python3 tests/integration.py ./build/cppbuild
sanitize:
	mkdir -p build
	$(CXX) $(CPPFLAGS) -std=c++17 -g -O1 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer tests/unit.cpp $(CORE) -o build/unit-sanitize
	./build/unit-sanitize
	$(CXX) $(CPPFLAGS) -std=c++17 -g -O1 -Wall -Wextra -Wpedantic -pthread -fsanitize=address,undefined -fno-omit-frame-pointer src/main.cpp $(CORE) -o build/cppbuild-sanitize
	python3 tests/integration.py ./build/cppbuild-sanitize
demo: build/cppbuild
	./build/cppbuild build --file examples/hello/Cppbuildfile --jobs 4 --explain
	./examples/hello/.cppbuild/bin/hello
benchmark: build/cppbuild
	python3 scripts/benchmark.py ./build/cppbuild
