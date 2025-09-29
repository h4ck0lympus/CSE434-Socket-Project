CC = gcc
CFLAGS = -Wall -Wextra -O3

all: manager disk user

debug: CFLAGS := -Wall -Wextra -O0 -ggdb
debug: clean all

manager: manager.c manager.h logging.h
	$(CC) $(CFLAGS) $< -o $@

disk: disk.c disk.h logging.h
	$(CC) $(CFLAGS) $< -o $@

user: user.c user.h logging.h
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all clean debug
clean:
	rm -f manager disk user 
