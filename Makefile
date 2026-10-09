CXX := clang++
CXXFLAGS := -std=c++20 -Wall -Wextra -Wpedantic -g -MMD -MP -Iinclude -Isrc
BUILD := build

LIB_OBJ := $(patsubst src/%.cpp,$(BUILD)/src/%.o,$(wildcard src/*.cpp))
CLI_OBJ := $(BUILD)/tools/mmapkv.o
TEST_SRC := $(wildcard tests/*.cpp)
TEST_OBJ := $(TEST_SRC:tests/%.cpp=$(BUILD)/tests/%.o)
TEST_BIN := $(TEST_SRC:tests/%.cpp=$(BUILD)/tests/%)
DEPS := $(LIB_OBJ:.o=.d) $(CLI_OBJ:.o=.d) $(TEST_OBJ:.o=.d)

.PHONY: all test sanitize clean

all: $(BUILD)/mmapkv $(TEST_BIN)

$(BUILD)/mmapkv: $(LIB_OBJ) $(CLI_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(TEST_BIN): $(BUILD)/tests/%: $(BUILD)/tests/%.o $(LIB_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

test: $(TEST_BIN)
	@for t in $(TEST_BIN); do ./$$t || exit 1; done

sanitize:
	$(MAKE) BUILD=build-asan CXXFLAGS="$(CXXFLAGS) -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer" all test

clean:
	rm -rf build build-asan

-include $(DEPS)
