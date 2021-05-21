CC=gcc
LD=$(CC)
CFLAGS=-O0 -g -ggdb -Wall -Wextra -pedantic -I /usr/include/mysql
LDFLAGS=-lmariadb -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

db: db.o
	$(LD) $(LDFLAGS) $^ -o $@

all: db

clean:
	rm -rf db
	rm -rf *.o
