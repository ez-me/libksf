# ksflate: the KZIP/PNGOUT deflate engine, its public C API and the libksf
# zlib-ABI shim.
#
#   make            build everything
#   make test       run the smoke tests (round-trip, size, ABI)
#   make clean
#
# Outputs:
#   build/libksf.so      zlib deflate-side shim for LD_PRELOAD
#   build/libksflate.a   C API (include/ksflate.h), static
#   build/libksflate.so  C API, shared
#
# The shim is compiled with hidden visibility; only the zlib entry points
# defined in zlib/ksf.cpp are exported there. The C API exports only the two
# KSFLATE_API functions. The engine sources are copied verbatim from the
# reference reconstruction (kzip-decomp/src) so this fork can evolve
# independently.

CXX      ?= g++
CC       ?= cc
AR       ?= ar
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -fPIC -fvisibility=hidden
CPPFLAGS ?= -Isrc -Iinclude
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
PUBLIC_OBJECTS := $(ENGINE_OBJECTS) $(BUILD)/ksflate_api.o
DEPS := $(OBJECTS:.o=.d) $(PUBLIC_OBJECTS:.o=.d)

.PHONY: all test clean

all: $(BUILD)/libksf.so $(BUILD)/libksflate.a $(BUILD)/libksflate.so \
	$(BUILD)/abi_test $(BUILD)/stream_abi $(BUILD)/c_api_test \
	$(BUILD)/concurrent $(BUILD)/history

$(BUILD)/libksf.so: $(OBJECTS) zlib/ksf.map
	$(CXX) -shared -Wl,-soname,libksf.so \
		-Wl,--version-script=zlib/ksf.map $(LDFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

$(BUILD)/libksflate.a: $(PUBLIC_OBJECTS)
	$(AR) rcs $@ $^

$(BUILD)/libksflate.so: $(PUBLIC_OBJECTS) include/ksflate.map
	$(CXX) -shared -Wl,-soname,libksflate.so \
		-Wl,--version-script=include/ksflate.map $(LDFLAGS) -o $@ \
		$(PUBLIC_OBJECTS)

$(BUILD)/%.o: src/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/ksflate_api.o: src/ksflate_api.cpp include/ksflate.h

$(BUILD)/ksf.o: zlib/ksf.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/abi_test: tests/abi.c $(BUILD)/libksf.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -lz

$(BUILD)/stream_abi: tests/stream_abi.c $(BUILD)/libksf.so
	$(CC) -O2 -Wall -Wextra -o $@ $< -lz

$(BUILD)/c_api_test: tests/c_api.c $(BUILD)/libksflate.a include/ksflate.h
	$(CC) -O2 -Wall -Wextra -Iinclude -c -o $(BUILD)/c_api_test.o $<
	$(CXX) -o $@ $(BUILD)/c_api_test.o $(BUILD)/libksflate.a -lz

$(BUILD)/concurrent: tests/concurrent.cpp $(ENGINE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -pthread -o $@ $^

$(BUILD)/history: tests/history.cpp $(ENGINE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -o $@ $^ -lz

test: all
	python3 tests/smoke.py
	$(BUILD)/concurrent
	$(BUILD)/history
	$(BUILD)/c_api_test

clean:
	rm -rf $(BUILD)

-include $(DEPS)
