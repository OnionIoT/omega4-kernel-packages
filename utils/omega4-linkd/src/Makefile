CC ?= cc
CFLAGS ?= -Os
LDFLAGS ?=

all: omega4-linkd omega4-mavlinkd

omega4-linkd: omega4-linkd.c
	$(CC) $(CFLAGS) -Wall -Wextra -std=c99 -o $@ $< $(LDFLAGS)

omega4-mavlinkd: omega4-mavlinkd.c
	$(CC) $(CFLAGS) -Wall -Wextra -std=c99 -o $@ $< $(LDFLAGS)

clean:
	rm -f omega4-linkd omega4-mavlinkd
