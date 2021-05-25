CC=gcc
LD=$(CC)
CFLAGS=-O0 -g -ggdb -Wall -Wextra -pedantic -I /usr/include/mysql
LDFLAGS=-lpthread
LDFLAGS_CLIENT=-lmariadb $(LDFLAGS)

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
