BIN_DIR = bin

all: buffer_lib.so test

bin:
	mkdir -p $(BIN_DIR)

buffer_lib.so: buffer_lib.c | bin
	gcc -shared -fPIC -Wall -o $(BIN_DIR)/buffer_lib.so buffer_lib.c -ldl

test: test.c | bin
	gcc -Wall -o $(BIN_DIR)/test test.c
clean:
	rm -rf $(BIN_DIR)