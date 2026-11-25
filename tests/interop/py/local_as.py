import base64
import hashlib
import json
import secrets
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

PORT = int(sys.argv[1])
MCP_URL = sys.argv[2]
PARSED = urllib.parse.urlparse(MCP_URL)
MCP_ORIGIN = PARSED.scheme + "://" + PARSED.netloc
ISSUER = f"http://127.0.0.1:{PORT}"
codes = {}
log = None


def emit(tag, payload):
    safe = json.dumps(payload, ensure_ascii=False).replace("t23-access-token", "<REDACTED>")
    print(f"AS_WIRE {tag} {safe}", file=log or sys.stderr, flush=True)


def challenge(verifier):
    return base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()


def resource_ok(resource):
    return not resource or resource.rstrip("/") in (MCP_URL.rstrip("/"), MCP_ORIGIN.rstrip("/"))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        pass

    def json_response(self, status, body):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def redirect(self, location):
        self.send_response(302)
        self.send_header("Location", location)
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        params = urllib.parse.parse_qs(parsed.query)
        emit("GET", {"path": parsed.path, "query_keys": sorted(params)})
        if parsed.path == "/.well-known/oauth-authorization-server":
            self.json_response(200, {
                "issuer": ISSUER,
                "authorization_endpoint": ISSUER + "/authorize",
                "token_endpoint": ISSUER + "/token",
                "registration_endpoint": ISSUER + "/register",
                "response_types_supported": ["code"],
                "code_challenge_methods_supported": ["S256"],
                "scopes_supported": ["mcp:tools"],
                "authorization_response_iss_parameter_supported": True,
                "grant_types_supported": ["authorization_code"],
                "token_endpoint_auth_methods_supported": ["none", "client_secret_post"],
            })
            return
        if parsed.path == "/authorize":
            redirect_uri = params.get("redirect_uri", [""])[0]
            state = params.get("state", [""])[0]
            cc = params.get("code_challenge", [""])[0]
            method = params.get("code_challenge_method", [""])[0]
            resource = params.get("resource", [""])[0]
            if not redirect_uri or not cc or method != "S256" or not resource_ok(resource):
                self.json_response(400, {"error": "invalid_request"})
                return
            code = "code-" + secrets.token_hex(12)
            codes[code] = {"challenge": cc, "resource": resource}
            sep = "&" if "?" in redirect_uri else "?"
            location = redirect_uri + sep + urllib.parse.urlencode({"code": code, "state": state, "iss": ISSUER})
            emit("AUTHORIZE", {"redirect": "callback", "has_code": True, "has_state": bool(state), "iss": ISSUER})
            self.redirect(location)
            return
        self.json_response(404, {"error": "not_found"})

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(length).decode() if length else ""
        form = urllib.parse.parse_qs(raw)
        emit("POST", {"path": parsed.path, "keys": sorted(form)})
        if parsed.path == "/register":
            self.json_response(201, {
                "client_id": "client-" + secrets.token_hex(8),
                "client_secret": "secret-" + secrets.token_hex(8),
                "redirect_uris": ["http://127.0.0.1:32399/callback"],
                "token_endpoint_auth_method": "client_secret_post",
                "grant_types": ["authorization_code"],
                "response_types": ["code"],
            })
            return
        if parsed.path == "/token":
            code = form.get("code", [""])[0]
            verifier = form.get("code_verifier", [""])[0]
            grant = form.get("grant_type", [""])[0]
            resource = form.get("resource", [""])[0]
            entry = codes.get(code)
            if grant != "authorization_code" or not entry:
                self.json_response(400, {"error": "invalid_grant"})
                return
            if challenge(verifier) != entry["challenge"] or not resource_ok(resource):
                self.json_response(400, {"error": "invalid_grant"})
                return
            codes.pop(code, None)
            self.json_response(200, {
                "access_token": "t23-access-token",
                "token_type": "Bearer",
                "expires_in": 3600,
                "scope": "mcp:tools",
                "resource": resource or MCP_URL,
            })
            return
        self.json_response(404, {"error": "not_found"})


if __name__ == "__main__":
    if len(sys.argv) > 3:
        log = open(sys.argv[3], "w", encoding="utf-8")
    ThreadingHTTPServer(("127.0.0.1", PORT), Handler).serve_forever()
