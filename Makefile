CC = gcc
CFLAGS = -Wall -Wextra -O3

all: manager

debug: CFLAGS := -Wall -Wextra -O0 -ggdb
debug: clean manager

manager: manager.c manager.h logging.h
	$(CC) $(CFLAGS) $< -o $@

.PHONY: all clean debug
clean:
	rm -f manager
