# isql — a minimal AI-driven database in C

A tiny SQLite-like embedded database where the **LLM is the SQL parser** and C is the
engine. You type a request in natural language (or SQL) — an LLM translates it into a
compact JSON *query plan* — and a hand-written C engine executes it against stored
data. There is **no SQL parser in the binary at all**.

```
you:  "show me users older than 26, newest first"
        │
        ▼  LLM (with the current schema in the system prompt)
{"op":"select","table":"users","columns":"*",
 "where":{"age":{"gt":26}},"order_by":[{"column":"age","desc":true}]}
        │
        ▼  C engine (db.c)
| id | name  | age |
+----+-------+-----+
| 3  | carol | 40  |
| 1  | alice | 30  |
```

## Architecture

| file        | role                                                        |
|-------------|--------------------------------------------------------------|
| `json.c/h`  | tiny recursive-descent JSON parser + serializer (wire format)|
| `db.c/h`    | the database engine: tables/rows in memory, WHERE evaluation,|
|             | type coercion, primary keys, ORDER BY/LIMIT, JSON persistence |
| `llm.c/h`   | OpenAI-compatible `/chat/completions` client (libcurl)       |
| `main.c`    | REPL: bundles the schema into a system prompt, calls the LLM,|
|             | plans via `db_exec`, pretty-prints results, saves the db     |

## Build & run

Requirements: a C compiler, [libcurl](https://curl.se/libcurl/) (`pkg-config libcurl`).
On macOS `libcurl` ships with the OS; on Linux e.g. `apt install libcurl4-openssl-dev`.

```sh
make            # builds ./isql
make test       # offline engine unit tests (no LLM, no libcurl)
```

### With a real LLM (OpenAI-compatible)

```sh
export ISQL_KEY=sk-...                          # or LLM_BASE for Ollama/etc
ISQL_BASE=https://api.openai.com/v1 \
ISQL_MODEL=gpt-4o-mini \
ISQL_DB=my.db.json \
./isql
```

Any OpenAI-compatible endpoint works, e.g. local Ollama:

```sh
ISQL_BASE=http://localhost:11434/v1 ISQL_MODEL=llama3.2 ./isql
```

### Offline demo / tests (no API key)

```sh
ISQL_FAKE_LLM=1 ./isql        # canned plans instead of a network call
```

A mock OpenAI server is also included for exercising the real HTTP path:

```sh
python3 tests/mock_llm.py 8765 &          # fake /v1/chat/completions
ISQL_BASE=http://127.0.0.1:8765/v1 ISQL_MODEL=mock ISQL_DB=/tmp/d.json ./isql
```

## REPL usage

Statements end with `;` **or** by pressing Enter on an empty line:

```
isql> create a table users with id (primary int), name (text), age (int)
isql> insert alice who is 30        <Enter>
                                       <Enter>      ← empty line finishes the statement
```

Example conversation (all go through the LLM):

```
create a table products with id name price
add a phone costing 699.99
list products cheaper than 800
mark the phone as discontinued
delete discontinued products
show tables / describe products
```

Meta commands (handled locally): `help`, `exit`/`quit`.

## The query plan (what the LLM speaks)

```jsonc
{"op":"create_table","table":"users","columns":[
    {"name":"id","type":"int","primary":true},
    {"name":"name","type":"text"},
    {"name":"score","type":"real"}]}
{"op":"drop_table","table":"users"}
{"op":"insert","table":"users","values":{"name":"alice","score":9.5}}   // pk auto-increments
{"op":"select","table":"users","columns":["name","score"],
 "where":{"score":{"gt":5}},
 "order_by":[{"column":"score","desc":true}],
 "limit":10}
{"op":"update","table":"users","values":{"score":10},"where":{"name":"alice"}}
{"op":"delete","table":"users","where":{"score":{"lt":3}}}
{"op":"show_tables"}  {"op":"describe","table":"users"}
{"op":"error","message":"..."}          // LLM reports it cannot handle the request
```

`where` grammar (nestable): `{"col": v}` equality, `{"col":{"gt":v}}` with
`gt|gte|lt|lte|ne`, plus `{"and":[...]}` / `{"or":[...]}`.

Types are enforced at write time (`int`, `real`, `text`); primary keys are unique and
auto-increment when omitted.

## Persistence

The whole database is a JSON document (tables → columns → rows). It loads from
`ISQL_DB` at startup and is rewritten atomically (`.tmp` + rename) after every
successful mutation.

## Scope / limits

This is a learning project, not production software:

- No indexes — full scans only (fine for tiny tables).
- No joins, aggregations or transactions.
- No SQL parser of its own — that's the point: the LLM does the parsing, so
  correctness depends on the model following the plan contract (JSON-schema-style
  prompt in `main.c::LLM_RULES`).
- Requires network + API key for the real path (or `ISQL_FAKE_LLM`/mock offline).

## Files

```
main.c      REPL, system prompt, result printing      db.c/h   engine, WHERE, persistence
json.c/h    JSON parser/serializer                    llm.c/h  OpenAI-compatible HTTP client
tests/      test_engine.c (offline units), mock_llm.py (fake server)
```