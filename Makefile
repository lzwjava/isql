CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")

SRC      = json.c db.c llm.c wal.c btree.c
OBJ      = $(SRC:.c=.o)

all: isql

isql: $(OBJ) main.o
	$(CC) $(CFLAGS) -o $@ $^ $(CURL_LIBS)

# engine + json only (no libcurl) — lets the engine be unit-tested offline
test_engine: json.o db.o wal.o btree.o tests/test_engine.o
	$(CC) $(CFLAGS) -o $@ $^

test_btree: btree.o json.o tests/test_btree.o
	$(CC) $(CFLAGS) -o $@ $^

bench: json.o db.o wal.o btree.o tests/bench.o
	$(CC) $(CFLAGS) -o $@ $^

main.o: main.c db.h json.h llm.h wal.h
db.o:   db.c db.h json.h wal.h
json.o: json.c json.h
llm.o:  llm.c llm.h json.h
wal.o:  wal.c wal.h db.h json.h
btree.o: btree.c btree.h json.h
tests/test_btree.o: tests/test_btree.c btree.h json.h
tests/bench.o: tests/bench.c db.h json.h

clean:
	rm -f *.o isql test_engine tests/test_engine.o

.PHONY: all clean test benchmarks
test: isql test_engine test_btree
	./test_engine
	./test_btree

benchmarks: bench
	./bench