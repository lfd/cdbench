CC=gcc
LD=$(CC)
LDFLAGS=-lpthread
LDFLAGS_CLIENT=-lmariadb $(LDFLAGS)

CFLAGS_ALL=-Wall -Wextra -pedantic -I /usr/include/mysql
CFLAGS_DEBUG=-O0 -g
CFLAGS_RELEASE=-O3

CFLAGS=$(CFLAGS_ALL) $(CFLAGS_RELEASE)

all: db server

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

db: db.o
	$(LD) $(LDFLAGS_CLIENT) $^ -o $@

server: server.o
	$(LD) $(LDFLAGS) $^ -o $@

clean:
	rm -rf db server
	rm -rf *.o
