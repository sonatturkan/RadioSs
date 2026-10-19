#!/bin/bash
# Refresh the windows-port-notes branch from /opt/radios-port (no working-tree changes).
set -e
N=/tmp/notes-wt; rm -rf $N; mkdir -p $N
git -C /home/user/RadioSs --work-tree=$N checkout windows-port-notes -- . 2>/dev/null || true
git -C /home/user/RadioSs reset -q 2>/dev/null || true
cp -r /opt/radios-port/specs/* $N/specs/; mkdir -p $N/reports; cp -r /opt/radios-port/reports/* $N/reports/
cp /opt/radios-port/bin/{wrun,wine-env.sh,ucrt64-tool,x86_64-w64-mingw32-gcc,x86_64-w64-mingw32-windres,x86_64-w64-mingw32-pkg-config} $N/cloud-env/bin/
cp /opt/radios-port/wine-support/*.c /opt/radios-port/wine-support/*.def /opt/radios-port/wine-support/test-env.reg $N/cloud-env/wine-support/
cp /opt/radios-port/setup/* $N/cloud-env/setup/; cp /opt/radios-port/collect.py /opt/radios-port/update-notes.sh $N/cloud-env/setup/
mkdir -p $N/workflows/cloud; cp /root/.claude/projects/-home-user-RadioSs/0a18ba4f-bd4a-5dde-9917-47df8cf688f0/workflows/scripts/*.js /tmp/claude-0/-home-user-RadioSs/0a18ba4f-bd4a-5dde-9917-47df8cf688f0/scratchpad/*.js $N/workflows/cloud/ 2>/dev/null || true
cd /home/user/RadioSs
export GIT_INDEX_FILE=/tmp/notes.index; rm -f $GIT_INDEX_FILE
git --work-tree=$N add -A .
T=$(git write-tree); P=$(git rev-parse windows-port-notes)
if [ "$T" != "$(git rev-parse windows-port-notes^{tree})" ]; then
  C=$(printf 'Notlar: güncel raporlar ve ortam betikleri\n\nCo-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>\nClaude-Session: https://claude.ai/code/session_01QbB1RMKGxba2vxf7Hk3KEw\n' | git commit-tree $T -p $P)
  git update-ref refs/heads/windows-port-notes $C
fi
unset GIT_INDEX_FILE
git push -q origin windows-port-notes
git log --oneline -1 windows-port-notes
