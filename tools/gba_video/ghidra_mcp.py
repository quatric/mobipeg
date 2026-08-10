#!/usr/bin/env python3
"""Minimal MCP client for Ghidra's plugin server over Streamable HTTP (/mcp).

The mcp__ghidra__* harness tools return empty errors (broken SSE bridge), so
talk to the Jetty endpoint directly.
"""
import json, urllib.request

URL = "http://127.0.0.1:8080/mcp"


class Ghidra:
    def __init__(self, url=URL):
        self.url = url
        self.sid = None
        self.n = 0

    def _post(self, payload, timeout=120):
        body = json.dumps(payload).encode()
        req = urllib.request.Request(self.url, data=body, method="POST")
        req.add_header("Content-Type", "application/json")
        req.add_header("Accept", "application/json, text/event-stream")
        if self.sid:
            req.add_header("Mcp-Session-Id", self.sid)
        with urllib.request.urlopen(req, timeout=timeout) as r:
            got_sid = r.headers.get("Mcp-Session-Id")
            if got_sid:
                self.sid = got_sid
            raw = r.read().decode(errors="replace")
        # Streamable HTTP may answer as SSE framing; pull out the data: lines.
        if raw.lstrip().startswith("event:") or "\ndata:" in raw or raw.startswith("data:"):
            out = []
            for line in raw.splitlines():
                if line.startswith("data:"):
                    out.append(line[5:].strip())
            raw = "\n".join(out)
        if not raw.strip():
            return None
        try:
            return json.loads(raw)
        except json.JSONDecodeError:
            return {"_raw": raw}

    def rpc(self, method, params=None, timeout=120):
        self.n += 1
        return self._post({"jsonrpc": "2.0", "id": self.n, "method": method,
                           "params": params or {}}, timeout=timeout)

    def notify(self, method, params=None):
        return self._post({"jsonrpc": "2.0", "method": method,
                           "params": params or {}})

    def connect(self):
        r = self.rpc("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "caimans-re", "version": "0"},
        })
        self.notify("notifications/initialized")
        return r

    def tools(self):
        return self.rpc("tools/list")

    def call(self, name, args=None, timeout=180):
        return self.rpc("tools/call", {"name": name, "arguments": args or {}},
                        timeout=timeout)


def text_of(resp):
    """Flatten a tools/call result into plain text."""
    if not resp:
        return ""
    if "error" in resp:
        return f"ERROR: {resp['error']}"
    content = resp.get("result", {}).get("content", [])
    return "\n".join(c.get("text", "") for c in content if isinstance(c, dict))


if __name__ == "__main__":
    g = Ghidra()
    init = g.connect()
    print("initialize:", json.dumps(init)[:400])
    tl = g.tools()
    names = [t["name"] for t in tl.get("result", {}).get("tools", [])]
    print(f"\n{len(names)} tools:")
    for t in tl.get("result", {}).get("tools", []):
        desc = (t.get("description") or "").split("\n")[0][:95]
        print(f"  {t['name']:38s} {desc}")
