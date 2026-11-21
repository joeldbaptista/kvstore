CC	= cc
CFLAGS	= -std=c99 -Wall -Wextra -pedantic -O2
OFILES	= kvblob.o example.o
HFILES	= blob.h kvblob.h

all: kvb

kvb: $(OFILES)
	$(CC) $(CFLAGS) -o $@ $(OFILES)

$(OFILES): $(HFILES)

clean:
	rm -f kvb $(OFILES) demo.kvb
