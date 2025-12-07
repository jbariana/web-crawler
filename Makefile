all: 
	gcc-std=c11-pedantic-pthread-lcurl crawler.c path.c -o crawler

run:
	 ./crawler

clean: 
	rm -f crawler

