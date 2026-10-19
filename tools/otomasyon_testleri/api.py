"""Yerel RadioS veri servisine kimlikli istemci (config/credentials.json)."""
import json, os, time, uuid, urllib.error, urllib.request

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CRED = json.load(open(os.path.join(ROOT, "config", "credentials.json")))
TOKEN, SID = CRED["api_token"], CRED["station_id"]
BASE = "http://127.0.0.1:8000"
# RADIOS_E2E_PACE_S=1: canlı servisi yormamak için istekler arasında en az bu kadar saniye bekle (varsayılan 0).
PACE_S = float(os.environ.get("RADIOS_E2E_PACE_S", "0") or 0)
_last_call = 0.0


def call(workflow, **body):
    global _last_call
    if PACE_S > 0:
        wait = _last_call + PACE_S - time.monotonic()
        if wait > 0:
            time.sleep(wait)
        _last_call = time.monotonic()
    data = json.dumps(body).encode()
    req = urllib.request.Request(BASE + "/v1/workflows/" + workflow, data=data, method="POST",
                                 headers={"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            return json.loads(r.read() or b"null")
    except urllib.error.HTTPError as e:
        raise SystemExit("HTTP %s %s: %s" % (e.code, workflow, e.read().decode()[:2000]))


def key():
    return str(uuid.uuid4())


def docs(kind):
    return call("desktop/list", station_id=SID, kind=kind)["records"]


def put(kind, name, config, document_id=None, expected_revision=None):
    body = dict(station_id=SID, kind=kind, name=name, config=config, idempotency_key=key())
    if document_id:
        body.update(document_id=document_id, expected_revision=expected_revision)
    return call("desktop/put", **body)


def delete(kind, document_id, expected_revision):
    return call("desktop/delete", station_id=SID, kind=kind, document_id=document_id,
                expected_revision=expected_revision, idempotency_key=key())
