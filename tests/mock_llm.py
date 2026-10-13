#!/usr/bin/env python3
"""mock_llm.py — a tiny fake OpenAI /chat/completions server.

It inspects the last user message and returns a canned JSON query plan,
so the whole isql pipeline can be tested without a real LLM/API key.

Usage:  python3 tests/mock_llm.py [port]   (default 8765)
"""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

def plan_for(user_msg: str) -> dict:
    u = user_msg.lower().strip()
    if "create" in u and "orders" in u:
        return {
            "op": "create_table",
            "table": "orders",
            "columns": [
                {"name": "id", "type": "int", "primary": True},
                {"name": "user_id", "type": "int"},
                {"name": "total", "type": "real"},
            ],
        }
    if "create" in u:
        return {
            "op": "create_table",
            "table": "users",
            "columns": [
                {"name": "id", "type": "int", "primary": True},
                {"name": "name", "type": "text"},
                {"name": "age", "type": "int"},
                {"name": "city", "type": "text"},
            ],
        }
    if "insert" in u or "add" in u:
        return {"op": "insert", "table": "users", "values": {"name": "alice", "age": 30, "city": "nyc"}}
    if "update" in u:
        return {"op": "update", "table": "users", "values": {"age": 31}, "where": {"name": "alice"}}
    if "delete" in u:
        return {"op": "delete", "table": "users", "where": {"age": {"gt": 40}}}
    if "join" in u or "buyer" in u or "customer" in u:
        return {
            "op": "select",
            "table": "orders",
            "join": {"table": "users", "on": {"users.id": "orders.user_id"}},
            "columns": ["users.name", "orders.total"],
            "where": {"orders.total": {"gt": 50}},
            "order_by": [{"column": "orders.total", "desc": True}],
        }
    if any(k in u for k in ("count", "average", "avg", "sum", "group", "per city")):
        return {
            "op": "aggregate",
            "table": "users",
            "group_by": ["city"],
            "aggs": [
                {"as": "n", "func": "count", "column": "*"},
                {"as": "avg_age", "func": "avg", "column": "age"},
            ],
        }
    if "add column" in u or "new column" in u:
        return {"op": "alter_table", "table": "users", "action": "add_column",
                "column": {"name": "email", "type": "text"}}
    if "batch" in u:
        return {
            "ops": [
                {"op": "insert", "table": "users", "values": {"name": "eve", "age": 29, "city": "la"}},
                {"op": "insert", "table": "users", "values": {"name": "frank", "age": 35, "city": "nyc"}},
            ]
        }
    if "show" in u or "tables" in u:
        return {"op": "show_tables"}
    return {"op": "select", "table": "users", "columns": "*",
            "where": {}, "order_by": [{"column": "age", "desc": True}]}

class Handler(BaseHTTPRequestHandler):
    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0))
        body = json.loads(self.rfile.read(length) or b"{}")
        messages = body.get("messages", [])
        user_msg = ""
        for m in messages:
            if m.get("role") == "user":
                user_msg = m.get("content", "")
        plan = plan_for(user_msg)
        plan_str = json.dumps(plan)
        streaming = bool(body.get("stream"))
        if not streaming:
            payload = json.dumps(
                {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion",
                    "choices": [
                        {
                            "index": 0,
                            "message": {"role": "assistant", "content": plan_str},
                            "finish_reason": "stop",
                        }
                    ],
                }
            ).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return
        # streamed (SSE) response: split the plan into a few deltas
        chunks = []
        for i in range(0, len(plan_str), max(1, len(plan_str) // 4 or 1)):
            chunks.append(plan_str[i : i + max(1, len(plan_str) // 4 or 1)])
        body_lines = [
            "data: " + json.dumps({"choices": [{"delta": {"role": "assistant"}}]}),
        ]
        for c in chunks:
            body_lines.append(
                "data: " + json.dumps({"choices": [{"delta": {"content": c}}]})
            )
        body_lines.append("data: [DONE]")
        payload = ("\n\n".join(body_lines) + "\n\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock_llm] " + fmt % args + "\n")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()