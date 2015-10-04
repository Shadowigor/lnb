
all:
	gcc -g -O3 -lssl -lcrypto -pthread -o bin/lnb_fileindexer src/lnb_fileindexer.c
	cp src/lnb.sh bin/lnb

.PHONY: clean
clean:
	rm tmp/* bin/*
