"""Deterministic local AI protocols and malformed responses for SQL regressions."""
import argparse
import json
import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


class MockAIService(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port=0):
        super().__init__(("127.0.0.1", port), Handler)
        self.events = []
        self.lock = threading.Lock()


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_):
        pass

    def reply(self, value, status=200, raw=False):
        body = value if raw else json.dumps(value, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", 0)))
        try:
            request = json.loads(body)
        except ValueError:
            return self.reply({"error": "invalid JSON request"}, 400)
        mode = self.path.strip("/").split("/")[0]
        with self.server.lock:
            self.server.events.append({"path": self.path, "body": request,
                                       "content_type": self.headers.get("Content-Type")})
        if mode == "bad-json":
            return self.reply(b"not JSON", raw=True)
        if mode == "root-array":
            return self.reply([])
        if mode == "http-error":
            return self.reply({"error": "synthetic service failure"}, 400)
        native = "/api/" in self.path
        if self.path.endswith("/chat/completions") or self.path.endswith("/api/generate"):
            if native:
                if request.get("stream") is not False:
                    return self.reply({"error": "generation must be non-streaming"}, 400)
                if not self.headers.get("Content-Type", "").startswith("application/json"):
                    return self.reply({"error": "missing JSON content type"}, 400)
                text = request.get("prompt")
            else:
                text = request.get("messages", [{}])[-1].get("content")
            if mode == "request":
                text = json.dumps(request, ensure_ascii=False)
            if mode == "empty":
                text = ""
            if mode == "null":
                text = None
            if mode == "wrong-type":
                text = {"invalid": "completion must be a string"}
            if native:
                return self.reply({"response": text, "done": True})
            return self.reply({"choices": [{"message": {"content": text}}]})
        if self.path.endswith("/embeddings") or self.path.endswith("/api/embed"):
            inputs = request.get("input")
            if isinstance(inputs, dict):
                inputs = inputs.get("texts")
            if isinstance(inputs, str):
                inputs = [inputs]
            if not isinstance(inputs, list) or not inputs or any(not isinstance(x, str) for x in inputs):
                return self.reply({"error": "input must contain strings"}, 400)
            if mode == "fail-later" and any(x.startswith("row-") and int(x[4:]) >= 64 for x in inputs):
                return self.reply({"error": "synthetic later batch failure"}, 400)
            dimension = request.get("dimensions", request.get("parameters", {}).get("dimension", 768))
            if not isinstance(dimension, int) or not 0 < dimension <= 65536:
                return self.reply({"error": "invalid dimension"}, 400)
            rows = []
            for i, value in enumerate(inputs):
                match = re.fullmatch(r"row-(\d+)", value)
                seed = int(match[1]) if match else 0.125
                vector = [seed] + [0.125] * (dimension - 1)
                if mode == "non-numeric":
                    vector[0] = "not-a-number"
                if mode == "non-finite":
                    vector[0] = float("inf")
                if mode == "empty-vector":
                    vector = []
                if mode == "wrong-dimension" or (mode == "inconsistent" and i > 0):
                    vector = vector[:-1]
                rows.append({"index": i, "text_index": i, "embedding": vector})
            if mode == "reverse" and not native:
                rows.reverse()
            if mode == "duplicate" and len(rows) > 1:
                rows[-1]["index"] = rows[0]["index"]
            if mode == "out-of-range":
                rows[0]["index"] = len(rows)
            if mode == "missing":
                rows.pop()
            if native:
                return self.reply({"embeddings": [r["embedding"] for r in rows]})
            return self.reply({"data": {} if mode == "bad-data" else rows,
                               "output": {"embeddings": rows}})
        if self.path.endswith("/rerank"):
            inputs = request.get("input", {})
            documents = request.get("documents", inputs.get("documents", []))
            rows = []
            for i, document in enumerate(documents):
                if mode == "scores":
                    score = int(document.split("-")[-1])
                elif mode == "ties":
                    score = 1
                else:
                    score = 1.0 if "apple" in document else 0.5
                rows.append({"index": i, "relevance_score": score})
            rows.reverse()
            if mode == "duplicate" and len(rows) > 1:
                rows[-1]["index"] = rows[0]["index"]
            if mode == "out-of-range":
                rows[0]["index"] = len(documents)
            if mode == "negative-index":
                rows[0]["index"] = -1
            if mode == "non-numeric":
                rows[0]["relevance_score"] = "bad"
            if mode == "non-finite":
                rows[0]["relevance_score"] = float("inf")
            if mode == "missing":
                rows.pop()
            return self.reply({"results": rows, "output": {"results": rows}})
        return self.reply({"error": "unknown endpoint"}, 404)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8000)
    args = parser.parse_args()
    service = MockAIService(args.port)
    print(f"Mock AI service listening on {service.server_address}", flush=True)
    service.serve_forever()
