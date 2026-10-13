# isql — a minimal AI-driven database in C

A tiny SQLite-like embedded database where the **LLM is the SQL parser** and C is the
engine. You type a request in natural language (or SQL) — an LLM translates it into a
compact JSON *query plan* — and a hand-written C engine executes it against stored
data. There is **no SQL parser in the binary at all**.

Features: CREATE/DROP/ALTER TABLE, INSERT, UPDATE, DELETE, SELECT with `WHERE`
(equality, `gt/gte/lt/lte/ne`, `IN`, `LIKE`, `BETWEEN`, nested `and`/`or`),
`ORDER BY`, `LIMIT`, `DISTINCT`, **INNER JOIN**, **aggregations**
(`count/sum/avg/min/max` + `GROUP BY`), primary keys with auto-increment and
type enforcement, and **crash-safe durability**:

- a **write-ahead log** (`<db>.wal`) records every mutation with a sequence
  number and `fsync`s it before the statement is confirmed;
- the snapshot file carries the covered `seq`, so **crash recovery replays
  exactly the entries a checkpoint did not cover** — nothing is lost, nothing
  duplicated;
- **checkpoints** (on clean exit, on demand, or automatically when the log
  grows) fold the log into the snapshot and truncate it;
- `{"ops":[...]}` batches and interactive **`BEGIN`/`COMMIT`/`ROLLBACK`**
  transactions buffer their log entries and apply them atomically — a crash
  or rollback discards them entirely.

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
| `db.c/h`    | the database engine: tables/rows, WHERE, JOIN, aggregate,    |
|             | PKs, type coercion, durable effect logging, seq checkpoints  |
| `wal.c/h`   | write-ahead log: fsync'd mutation records, atomic batch      |
|             | buffering, crash replay, snapshot checkpointing              |
| `llm.c/h`   | OpenAI-compatible `/chat/completions` client (libcurl)       |
| `main.c`    | REPL: LLM system prompt, multi-turn memory, BEGIN/COMMIT/   |
|             | ROLLBACK, result printing, attach WAL + recover on startup   |

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

Meta commands (handled locally): `help`, `exit`/`quit`, and the
**transaction keywords** `begin` / `commit` / `rollback`:

```
isql> begin
BEGIN
isql> insert alice 30;
Inserted 1 row into 'users'
isql> insert bob 25;
Inserted 1 row into 'users'
isql> rollback
ROLLBACK                     ; both inserts undone, nothing written to disk
```

## The query plan (what the LLM speaks)

```jsonc
{"op":"create_table","table":"users","columns":[
    {"name":"id","type":"int","primary":true},
    {"name":"name","type":"text"}]}
{"op":"drop_table","table":"users"}
{"op":"alter_table","table":"users","action":"add_column",
 "column":{"name":"email","type":"text"}}                     // or drop_column
{"op":"insert","table":"users","values":{"name":"alice"}}     // pk auto-increments
{"op":"select","table":"users","columns":["name"],"where":{"name":{"like":"a%"}},
 "order_by":[{"column":"name","desc":true}],"limit":10,"distinct":true}

// INNER JOIN — "on" is a map of column references (either orientation works)
{"op":"select","table":"orders",
 "join":{"table":"users","on":{"users.id":"orders.user_id"}},
 "columns":["users.name","orders.total"],
 "where":{"orders.total":{"gt":100}}}

// aggregation
{"op":"aggregate","table":"users","group_by":["city"],
 "aggs":[{"as":"n","func":"count","column":"*"},
         {"as":"avg_age","func":"avg","column":"age"},
         {"as":"oldest","func":"max","column":"age"}]}

{"op":"update","table":"users","values":{"age":30},"where":{"name":"alice"}}
{"op":"delete","table":"users","where":{"age":{"lt":3}}}
{"op":"show_tables"}  {"op":"describe","table":"users"}
{"op":"error","message":"..."}    // LLM reports it cannot handle the request

// atomic batch: every op commits, or ALL of them roll back
{"ops":[{"op":"insert",...},{"op":"update",...}]}
```

`where` grammar (nestable): `{"col": v}` equality, `{"col":{"gt":v}}` with
`gt|gte|lt|lte|ne`, `{"col":{"in":[...]}}`, `{"col":{"like":"a%"}}`
(`%` = any sequence, `_` = one char), `{"col":{"between":[lo,hi]}}`, plus
`{"and":[...]}` / `{"or":[...]}`.

Types are enforced at write time (`int`, `real`, `text`); primary keys are unique and
auto-increment when omitted.

## Persistence

The whole database is a JSON document (tables → columns → rows). It loads from
`ISQL_DB` at startup and is rewritten atomically (`.tmp` + rename) after every
successful mutation.

## Scope / limits

This is a learning project, not production software:

- No indexes — full scans only (fine for tiny tables).
- LEFT/FULL joins, subqueries and transactions spanning multiple statements are
  not implemented (single-statement `{"ops":[...]}` batches are atomic though).
- `join` merges rows; columns are exposed as `"<table>.<col>"` when the bare
  name collides between the two tables.
- No SQL parser of its own — that's the point: the LLM does the parsing, so
  correctness depends on the model following the plan contract (JSON-schema-style
  prompt in `main.c::LLM_RULES`).
- Requires network + API key for the real path (or `ISQL_FAKE_LLM`/mock offline).

## Files

```
main.c      REPL, system prompt, multi-turn memory, printing    db.c/h   engine, JOIN, aggregate, WHERE, atomic batches, persistence
json.c/h    JSON parser/serializer                              llm.c/h  OpenAI-compatible HTTP client
tests/      test_engine.c (66 offline unit checks), mock_llm.py (fake OpenAI
server), crash_recovery.sh (kill -9 + end-to-end recovery)
```

The multi-turn memory keeps the last 6 (request -> plan) pairs in the system
prompt, so follow-ups like *"and those older than 40"* refer to earlier turns.