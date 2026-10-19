"""Minimal rate-limited client for the local RadioS data service."""
import json, time, uuid, urllib.error, urllib.request
ROOT = "/home/sturkan/Masaüstü/radyo projesi/kaynak kodlar/RadioS"
CRED = json.load(open(ROOT + "/config/credentials.json"))
TOKEN, SID = CRED["api_token"], CRED["station_id"]
BASE = "http://127.0.0.1:8000"
_last = [0.0]

def _wait():
    gap = time.time() - _last[0]
    if gap < 1.05: time.sleep(1.05 - gap)
    _last[0] = time.time()

def call(workflow, retries=6, **body):
    data = json.dumps(body).encode()
    for attempt in range(retries):
        _wait()
        req = urllib.request.Request(BASE + "/v1/workflows/" + workflow, data=data, method="POST",
                                     headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                return json.loads(r.read() or b"null")
        except urllib.error.HTTPError as e:
            text = e.read().decode(errors="replace")
            if e.code == 429 and attempt + 1 < retries:
                time.sleep(2 + attempt * 2); continue
            raise RuntimeError("HTTP %s %s: %s" % (e.code, workflow, text[:3000]))

def get(path):
    _wait()
    req = urllib.request.Request(BASE + path, headers={"Authorization": "Bearer " + TOKEN})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read() or b"null")

def key(): return str(uuid.uuid4())
def docs(kind): return call("desktop/list", station_id=SID, kind=kind)["records"]
