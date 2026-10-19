#!/usr/bin/env python3
"""Save agent results of a workflow journal and commit the files each finished area reported (local only)."""
import json, sys, os, subprocess
ROOT = '/home/user/RadioSs'
for j in sys.argv[1:]:
    for l in open(j):
        d = json.loads(l)
        r = d.get('result')
        if d.get('type') != 'result' or not isinstance(r, dict) or 'area' not in r:
            continue
        area = r['area']
        out = f'/opt/radios-port/reports/fast/{area}.json'
        if os.path.exists(out):
            continue
        json.dump(r, open(out, 'w'), ensure_ascii=False, indent=1)
        files = []
        for k in ('files_created', 'files_modified', 'files_deleted'):
            for f in r.get(k, []):
                f = f.strip().split(' ')[0]
                if f.startswith(ROOT + '/'):
                    f = f[len(ROOT) + 1:]
                if f and not f.startswith('/') and '*' not in f:
                    files.append(f)
        files = [f for f in dict.fromkeys(files)]
        if files:
            subprocess.run(['git', 'add', '-A', '--'] + files, cwd=ROOT, capture_output=True)
            msg = f"Windows taşıması: {area}\n\n{r.get('summary','')[:1500]}\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01QbB1RMKGxba2vxf7Hk3KEw\n"
            rc = subprocess.run(['git', 'commit', '-q', '-m', msg], cwd=ROOT, capture_output=True, text=True)
            print(area, 'committed' if rc.returncode == 0 else 'nothing-to-commit', len(files), 'files')
        else:
            print(area, 'no files changed')
# push the code branch after collecting (user asked for GitHub pushes)
for i in range(4):
    r = subprocess.run(['git', 'push', '-q', 'origin', 'windows-port'], cwd=ROOT, capture_output=True, text=True)
    if r.returncode == 0:
        print('pushed windows-port'); break
    import time; time.sleep(2 ** (i + 1))
else:
    print('PUSH FAILED', r.stderr[-300:])
