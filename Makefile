BIN_DIR = bin
SRC_DIR = src

# Last commit touching buffer_lib.c, plus -dirty if it has uncommitted changes
BUFFER_LIB_VERSION := $(shell git log -1 --format=%h -- $(SRC_DIR)/buffer_lib.c 2>/dev/null || echo unknown)$(shell git diff --quiet HEAD -- $(SRC_DIR)/buffer_lib.c 2>/dev/null || echo -dirty)

all: buffer_lib.so logger.so test benchmark

compression: compression_lib.so test_compression

bin:
	mkdir -p $(BIN_DIR)

buffer_lib.so: $(SRC_DIR)/buffer_lib.c | bin
	gcc -shared -fPIC -Wall -O2 -DBUFFER_LIB_VERSION='"$(BUFFER_LIB_VERSION)"' -o $(BIN_DIR)/buffer_lib.so $(SRC_DIR)/buffer_lib.c -ldl

logger.so: $(SRC_DIR)/logger.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/logger.so $(SRC_DIR)/logger.c -ldl

# Not in all: needs the libdeflate, zstd and zlib dev headers
compression_lib.so: $(SRC_DIR)/compression_lib.c | bin
	gcc -shared -fPIC -Wall -O2 -o $(BIN_DIR)/compression_lib.so $(SRC_DIR)/compression_lib.c -ldl -ldeflate

test: tests/test.c | bin
	gcc -Wall -o $(BIN_DIR)/test tests/test.c

test_compression: tests/test_compression.c | bin
	gcc -Wall -o $(BIN_DIR)/test_compression tests/test_compression.c -lzstd -lz

test_gz_read: tests/test_gz_read.c | bin
	gcc -Wall -O2 -o $(BIN_DIR)/test_gz_read tests/test_gz_read.c -lz

# Not in all: needs the zlib, libdeflate and zstd dev headers. The TractoFlow
# container has no zstd dev package, so bench_compression.sh overrides these
ZSTD_CFLAGS =
ZSTD_LIBS = -lzstd

bench_compression: tests/bench_compression.c | bin
	gcc -Wall -O2 $(ZSTD_CFLAGS) -o $(BIN_DIR)/bench_compression tests/bench_compression.c -lz -ldeflate $(ZSTD_LIBS)

benchmark: tests/benchmark.c | bin
	gcc -Wall -O2 -o $(BIN_DIR)/benchmark tests/benchmark.c

run: test
	./bin/test

bench: benchmark
	./tests/run.sh

clean:
	rm -rf $(BIN_DIR) tests/output