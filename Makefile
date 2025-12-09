CC = gcc
CFLAGS = -std=c11 -pedantic -Wall -Wextra -pthread
LIBS = -lcurl

all: crawler

crawler: crawler.o path.o
	$(CC) $(CFLAGS) crawler.o path.o -o crawler $(LIBS)

crawler.o: crawler.c path.h
	$(CC) $(CFLAGS) -c crawler.c

path.o: path.c path.h
	$(CC) $(CFLAGS) -c path.c

run:
	./crawler

clean:
	rm -f crawler *.o