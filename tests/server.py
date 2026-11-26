"""Tiny local test site for the browser's forms, cookies, redirects and layout. Run: python tests/server.py"""
import html
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

STYLE = """<style>
body{font-family:Segoe UI,Arial,sans-serif;margin:0;background:#f4f5f7;color:#222}
header{display:flex;align-items:center;justify-content:space-between;background:#243447;color:#fff;padding:10px 24px}
header nav{display:flex;gap:18px}
header a{color:#cfe3ff;text-decoration:none}
main{max-width:720px;margin:24px auto;padding:0 16px}
.card{background:#fff;border:1px solid #d9dde3;border-radius:12px;padding:18px 22px;margin-bottom:18px;position:relative}
.badge{position:absolute;top:12px;right:14px;background:#e8f0fe;color:#1a56c4;border-radius:10px;padding:2px 10px;font-size:12px}
.row{display:flex;gap:10px;align-items:center;margin:10px 0}
.row label{width:90px;color:#555}
.row input[type=text],.row input[type=password],.row select{flex:1;padding:7px 10px;border:1px solid #b8c0cc;border-radius:6px;font-size:14px}
button,input[type=submit]{background:#1a73e8;color:#fff;border:1px solid #1a73e8;border-radius:6px;padding:7px 16px;font-size:14px}
.tags span{display:inline-block;background:#eef1f5;border-radius:12px;padding:3px 12px;margin:2px 4px 2px 0;font-size:13px}
pre{background:#f0f2f5;padding:10px;border-radius:6px;white-space:pre-wrap}
.clip{width:180px;height:22px;overflow:hidden;border:1px dashed #999}
</style>"""

def page(title, body):
    return f"<!doctype html><html><head><meta charset=utf-8><title>{title}</title>{STYLE}</head><body>" \
           f"<header><b>Test Site</b><nav><a href='/'>Home</a><a href='/echo'>Cookies</a></nav></header><main>{body}</main></body></html>"

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def send(self, body, status=200, headers=()):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        for k, v in headers:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        cookies = self.headers.get("Cookie", "")
        if u.path == "/":
            self.send(page("Forms test", """
<div class=card><span class=badge>GET</span><h2>Search</h2>
<form action="/search" method="get"><div class=row><label>Query</label><input type=text name=q placeholder="Type and press Enter"><input type=submit value="Go"></div></form></div>
<div class=card><span class=badge>POST</span><h2>Sign in</h2>
<form action="/login" method="post">
<input type=hidden name=csrf value="tok123">
<div class=row><label>User</label><input type=text name=username></div>
<div class=row><label>Password</label><input type=password name=password></div>
<div class=row><label>Plan</label><select name=plan><option value=free>Free</option><option value=pro selected>Pro</option><option value=team>Team</option></select></div>
<div class=row><label></label><input type=checkbox name=remember value=yes checked> Remember me &nbsp; <input type=radio name=mode value=a checked> A <input type=radio name=mode value=b> B</div>
<div class=row><label>Notes</label><textarea name=notes rows=2 cols=30>hello</textarea></div>
<div class=row><label></label><button type=submit name=go value=1>Sign in</button></div>
</form></div>
<div class=card><h2>Layout bits</h2><p class=tags><span>inline-block</span><span>pills</span><span>wrap nicely</span></p>
<div class=clip>This long sentence is clipped by overflow hidden and must not spill out of its dashed box.</div></div>"""))
        elif u.path == "/search":
            self.send(page("Search results", f"<div class=card><h2>Results for: {html.escape(q.get('q', [''])[0])}</h2><p>Query string: <code>{html.escape(u.query)}</code></p></div>"))
        elif u.path == "/welcome":
            self.send(page("Welcome", f"<div class=card><h2>Signed in</h2><p>Cookie header received by server:</p><pre>{html.escape(cookies) or '(none)'}</pre></div>"))
        elif u.path == "/echo":
            self.send(page("Cookies", f"<div class=card><h2>Cookies</h2><pre>{html.escape(cookies) or '(none)'}</pre><p>Referer: {html.escape(self.headers.get('Referer', '(none)'))}</p></div>"))
        elif u.path == "/refresh":
            self.send("<!doctype html><html><head><meta http-equiv=refresh content='0; url=/echo'><title>Refreshing</title></head><body>Redirecting</body></html>")
        else:
            self.send(page("Not found", "<div class=card><h2>404</h2></div>"), 404)

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode("utf-8", "replace")
        f = parse_qs(raw)
        if self.path == "/login":
            user = f.get("username", [""])[0]
            ok = f.get("csrf", [""])[0] == "tok123" and self.headers.get("Origin", "") == "http://localhost:8765"
            fields = ",".join(sorted(f.keys()))
            self.send("", 303, [("Location", "/welcome"),
                                ("Set-Cookie", f"session=user-{user}-ok{int(ok)}; Path=/; Max-Age=3600; HttpOnly; SameSite=Lax"),
                                ("Set-Cookie", f"fields={fields.replace(',', '+')}; Path=/"),
                                ("Set-Cookie", f"plan={f.get('plan', [''])[0]}-{f.get('mode', [''])[0]}-{f.get('remember', ['no'])[0]}; Path=/")])
        else:
            self.send(page("Not found", ""), 404)

HTTPServer(("127.0.0.1", 8765), H).serve_forever()
