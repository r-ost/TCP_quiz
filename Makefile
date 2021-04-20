CC=gcc
CFLAGS=-std=gnu99 -Wall

all: server client

server: server.c
	${CC} -o server server.c ${CFLAGS}

client: client.c
	${CC} -o client client.c ${CFLAGS}

.PHONY: clean

clean:
	rm -f server client