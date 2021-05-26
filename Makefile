CC=gcc
LD=$(CC)
LDFLAGS=-lpthread
LDFLAGS_CLIENT=-lmariadb $(LDFLAGS)

CFLAGS_ALL=-Wall -Wextra -pedantic -I /usr/include/mysql
CFLAGS_DEBUG=-O0 -g
CFLAGS_RELEASE=-O3

CFLAGS=$(CFLAGS_ALL) $(CFLAGS_RELEASE)

TARGET=cdbench dummysqld

all: $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $^ -o $@

cdbench: cdbench.o
	$(LD) $(LDFLAGS_CLIENT) $^ -o $@

dummysqld: dummysqld.o
	$(LD) $(LDFLAGS) $^ -o $@

clean:
	rm -rf $(TARGET)
	rm -rf *.o
