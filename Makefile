
all:
	gcc -Wall -Wextra -pedantic -O3 -o bin/lnb_fileindexer src/lnb_fileindexer.c
	cp src/lnb.sh bin/lnb

install:
	cp bin/lnb bin/lnb_fileindexer /usr/bin/
	mkdir -p "/usr/share/lnb"
	cp scripts/* /usr/share/lnb

.PHONY: clean
clean:
	rm bin/*
