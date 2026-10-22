# wordfreq: a small multi-file C project for /bin/make and tcc.
#   make          build ./wordfreq          make test   run a quick self-check
#   make clean    remove the build output   make V=1    show the commands
CC     = tcc
CFLAGS = -Wall -O
SRCS   = main.c words.c table.c
OBJS   = $(SRCS:.c=.o)
HDRS   = $(wildcard *.h)
Q      = $(if $(V),,@)

all: wordfreq

wordfreq: $(OBJS)
	$(Q)echo LINK $@
	$(Q)$(CC) -o $@ $^

%.o: %.c $(HDRS)
	$(Q)echo CC $<
	$(Q)$(CC) $(CFLAGS) -c -o $@ $<

test: wordfreq
	@printf 'the cat the dog\nthe end\n' | ./wordfreq | head -1 | grep -q '3 the' && echo "test: ok"

clean:
	rm -f $(OBJS) wordfreq

.PHONY: all test clean
