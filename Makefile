BIN_DIR = bin
SRC_DIR = src

all: buffer_lib.so logger.so test benchmark

bin:
	mkdir -p $(BIN_DIR)

buffer_lib.so: $(SRC_DIR)/buffer_lib.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/buffer_lib.so $(SRC_DIR)/buffer_lib.c -ldl

logger.so: $(SRC_DIR)/logger.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/logger.so $(SRC_DIR)/logger.c -ldl

test: $(SRC_DIR)/test.c | bin
	gcc -Wall -o $(BIN_DIR)/test $(SRC_DIR)/test.c

benchmark: tests/benchmark.c | bin
	gcc -Wall -O2 -o $(BIN_DIR)/benchmark tests/benchmark.c

run: benchmark
	./tests/run.sh

clean:
	rm -rf $(BIN_DIR) tests/output