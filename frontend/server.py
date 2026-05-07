#!/usr/bin/env python3
import os
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.error import HTTPError, URLError
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


PORT = int(os.environ.get("SATMON_FRONTEND_PORT", "5173"))
API_TARGET = os.environ.get(
    "SATMON_API_TARGET",
    "https://n06cy09ved.execute-api.eu-west-1.amazonaws.com/dev/SatMonHTTP",
).rstrip("/")


class SatMonHandler(SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET,POST,OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "content-type,accept")
        # Never cache static assets so a refresh always serves the latest UI.
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def do_OPTIONS(self):
        self.send_response(204)
        self.end_headers()

    def do_GET(self):
        if self.path == "/api" or self.path.startswith("/api/") or self.path.startswith("/api?"):
            self.proxy_api("GET")
            return

        super().do_GET()

    def do_POST(self):
        if self.path == "/api" or self.path.startswith("/api/") or self.path.startswith("/api?"):
            self.proxy_api("POST")
            return

        self.send_error(404)

    def proxy_api(self, method):
        api_path = self.path[4:]
        if not api_path:
            api_path = ""

        body = None
        content_length = int(self.headers.get("Content-Length", "0") or 0)
        if content_length > 0:
            body = self.rfile.read(content_length)

        target_url = f"{API_TARGET}{api_path}"
        request = Request(
            target_url,
            data=body,
            headers={
                "Accept": self.headers.get("Accept", "application/json"),
                "Content-Type": self.headers.get("Content-Type", "application/json"),
                "User-Agent": "SatMon-Frontend-Proxy/1.0",
            },
            method=method,
        )

        try:
            with urlopen(request, timeout=20) as response:
                body = response.read()
                status = response.status
                headers = response.headers
        except HTTPError as error:
            body = error.read()
            status = error.code
            headers = error.headers
        except URLError as error:
            body = f'{{"error":"API proxy failed","message":"{error.reason}"}}'.encode()
            status = 502
            headers = {"Content-Type": "application/json"}

        self.send_response(status)
        self.send_header("Content-Type", headers.get("Content-Type", "application/json"))
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    server = ThreadingHTTPServer(("0.0.0.0", PORT), SatMonHandler)
    print(f"SatMon dashboard: http://localhost:{PORT}", flush=True)
    print(f"Frontend files: {os.getcwd()}", flush=True)
    print(f"API proxy: /api -> {API_TARGET}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
