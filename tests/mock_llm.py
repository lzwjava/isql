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
    if "create" in u:
        return {
            "op": "create_table",
            "table": "users",
            "columns": [
                {"name": "id", "type": "int", "primary": True},
                {"name": "name", "type": "text"},
                {"name": "age", "type": "int"},
            ],
        }
    if u.startswith("insert") or "insert" in u:
        return {"op": "insert", "table": "users", "values": {"name": "alice", "age": 30}}
    if "update" in u:
        return {"op": "update", "table": "users", "values": {"age": 31}, "where": {"name": "alice"}}
    if "delete" in u:
        return {"op": "delete", "table": "users", "where": {"age": {"gt": 40}}}
    if "show" in u or "tables" in u:
        return {"op": "show_tables"}
    # default: select everything
    return {"op": "select", "table": "users", "columns": "*", "where": {}, "order_by": [{"column": "age", "desc": True}]}

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
        payload = json.dumps(
            {
                "id": "chatcmpl-mock",
                "object": "chat.completion",
                "choices": [
                    {
                        "index": 0,
                        "message": {"role": "assistant", "content": json.dumps(plan)},
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

    def log_message(self, fmt, *args):
        sys.stderr.write("[mock_llm] " + fmt % args + "\n")

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()