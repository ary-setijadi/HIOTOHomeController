#!/usr/bin/env python3
"""Build an offline preview of one console view (real CSS + JS + live fixtures).

Renders what a browser receives from the server, with fetch() stubbed to serve
captured fixtures, so the page can be screenshotted without logging in. It is a
local artefact only and never touches the server.
"""
import http.cookiejar
import json
import sys
import urllib.request

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

BASE = "http://192.168.1.99:8082"
VIEW = sys.argv[1] if len(sys.argv) > 1 else "overview"

cj = http.cookiejar.CookieJar()
op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(cj))
op.open(BASE + "/login", data=b"user=admin&password=Hioto-OTA-2026")

fixtures = {}
for p in ("/api/overview", "/api/channels", "/api/fleet",
          "/api/events?limit=300", "/api/health"):
    fixtures[p] = json.load(op.open(BASE + p))

css = op.open(BASE + "/app.css").read().decode()
js = op.open(BASE + "/app.js").read().decode()
index = op.open(BASE + "/").read().decode()

# --- make the JS start on the requested view, with no auto-refresh ---
before = js
js = js.replace("location.hash = v;", "")
js = js.replace(
    "const fromHash = (location.hash || '').replace('#', '');",
    "const fromHash = " + json.dumps(VIEW) + ";")
js = js.replace("setInterval(", "noInterval(")
js = "function noInterval(){return 0;}\n" + js
if js == before:
    sys.exit("preview: view pin did not apply")
if "noInterval(" not in js:
    sys.exit("preview: refresh pin did not apply")

stub = """
(function(){
  const FIX = %s;
  window.fetch = async function(url){
    const key = Object.keys(FIX).find(k => url === k || url.indexOf(k) === 0);
    const body = key ? FIX[key] : {error:'no fixture '+url};
    return { status:200, ok:true,
             headers:{ get:()=>'application/json' },
             json: async()=>body, text: async()=>JSON.stringify(body) };
  };
})();
""" % json.dumps(fixtures)

js_name = f"_preview_js_{VIEW}.js"
with open(js_name, "w", encoding="utf-8") as fh:
    fh.write(stub + "\n" + js)

index = index.replace('<link rel="stylesheet" href="/app.css">',
                      "<style>\n" + css + "\n</style>")
index = index.replace('<script src="/app.js"></script>',
                      '<script src="%s"></script>' % js_name)

name = f"_preview_{VIEW}.html"
with open(name, "w", encoding="utf-8") as fh:
    fh.write(index)
print(f"wrote {name} (view={VIEW}, js={js_name})")
