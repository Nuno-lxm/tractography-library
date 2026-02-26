buffer_lib.so: buffer_lib.c
	gcc -shared -fPIC -Wall -o buffer_lib.so buffer_lib.c -ldl

test: test.c
	gcc -Wall -o test test.c

clean:
	rm -f buffer_lib.so