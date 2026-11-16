# libksf: a zlib-ABI one-shot compression shim over the KZIP/PNGOUT deflate
# engine.
#
#   make            build build/libksf.so and build/abi_test
#   make test       run the smoke tests (round-trip, size, ABI)
#   make clean
#
# The shim is compiled with hidden visibility; only the zlib entry points
# defined in zlib/ksf.cpp are exported. The engine sources are copied verbatim
# from the reference reconstruction (kzip-decomp/src) so this fork can evolve
# independently.

CXX      ?= g++
CC       ?= cc
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden
CPPFLAGS ?= -Isrc
LDFLAGS  ?=
LDLIBS   ?=
BUILD    ?= build

# Emit header dependencies so a changed struct layout cannot leave a stale
# object behind (the parser's scratch moved from statics into the class).
DEPFLAGS := -MMD -MP

ENGINE := \
	src/strategy0_encoder.cpp \
	src/strategy12_parser.cpp \
	src/strategy0_model.cpp \
	src/parser_cost.cpp \
	src/huffman.cpp \
	src/deflate_writer.cpp \
	src/block_splitter.cpp \
	src/split_score_model.cpp

OBJECTS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(ENGINE)) $(BUILD)/ksf.o
ENGINE_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/%.o,$(ENGINE))
DEPS := $(OBJECTS:.o=.d) $(ENGINE_OBJECTS:.o=.d)

.PHONY: all test clean

all: $(BUILD)/libksf.so $(BUILD)/abi_test $(BUILD)/stream_abi $(BUILD)/concurrent $(BUILD)/history

$(BUILD)/libksf.so: $(OBJECTS) zlib/ksf.map
	$(CXX) -shared -Wl,-soname,libksf.so \
		-Wl,--version-script=zlib/ksf.map $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/ksf.o: zlib/ksf.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/abi_test: tests/abi.c $(BUILD)/libksf.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -lz

$(BUILD)/stream_abi: tests/stream_abi.c $(BUILD)/libksf.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -lz

$(BUILD)/concurrent: tests/concurrent.cpp $(ENGINE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -o $@ $^

$(BUILD)/history: tests/history.cpp $(ENGINE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ -lz

test: all
	python3 tests/smoke.py
	$(BUILD)/concurrent
	$(BUILD)/history

clean:
	rm -rf $(BUILD)

-include $(DEPS)
