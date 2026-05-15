CC ?= cc
CFLAGS ?= -Os
LDFLAGS ?=

omega4-linkd: omega4-linkd.c
	$(CC) $(CFLAGS) -Wall -Wextra -std=c99 -o $@ $< $(LDFLAGS)

clean:
	rm -f omega4-linkd
