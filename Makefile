CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra
CURL_LIBS = $(shell pkg-config --libs libcurl 2>/dev/null || echo "-lcurl")

SRC      = json.c db.c llm.c
OBJ      = $(SRC:.c=.o)

all: isql

isql: $(OBJ) main.o
	$(CC) $(CFLAGS) -o $@ $^ $(CURL_LIBS)

# engine + json only (no libcurl) — lets the engine be unit-tested offline
test_engine: json.o db.o tests/test_engine.o
	$(CC) $(CFLAGS) -o $@ $^

main.o: main.c db.h json.h llm.h
db.o:   db.c db.h json.h
json.o: json.c json.h
llm.o:  llm.c llm.h json.h

clean:
	rm -f *.o isql test_engine tests/test_engine.o

.PHONY: all clean test
test: isql test_engine
	./test_engine