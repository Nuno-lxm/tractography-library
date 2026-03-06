BIN_DIR = bin
SRC_DIR = src

all: buffer_lib.so logger.so test

bin:
	mkdir -p $(BIN_DIR)

buffer_lib.so: $(SRC_DIR)/buffer_lib.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/buffer_lib.so $(SRC_DIR)/buffer_lib.c -ldl

logger.so: $(SRC_DIR)/logger.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/logger.so $(SRC_DIR)/logger.c -ldl

test: $(SRC_DIR)/test.c | bin
	gcc -Wall -o $(BIN_DIR)/test $(SRC_DIR)/test.c
	
clean:
	rm -rf $(BIN_DIR)