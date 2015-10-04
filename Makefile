
all:
	gcc -Wall -Wextra -pedantic -O3 -o bin/lnb_fileindexer src/lnb_fileindexer.c
	cp src/lnb.sh bin/lnb

install:
	cp bin/lnb bin/lnb_fileindexer /usr/bin/
	if [ ! -d "/usr/share/lnb" ]; then
		mkdir "/usr/share/lnb"
	fi
	cp scripts/* /usr/share/lnb

.PHONY: clean
clean:
	rm bin/*
