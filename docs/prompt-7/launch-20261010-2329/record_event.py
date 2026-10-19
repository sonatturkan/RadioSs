#!/usr/bin/env python3
from pathlib import Path
from datetime import datetime, timezone
from contextlib import ExitStack
import fcntl, os, json, sys
own=Path(__file__).resolve().parent
base=own.parents[3]
shared=base/'prompt/agent/AGENT.md'
shared.read_bytes()
if (own/'state.json').exists(): (own/'state.json').read_bytes()
event=json.load(sys.stdin)
now=datetime.now(timezone.utc);utc=now.strftime('%Y-%m-%d %H:%M:%S UTC')
event['utc']=utc;event['agent']='/root/launch_coordination'
statepath=own/'state.json'
state=json.loads(statepath.read_text()) if statepath.exists() else {}
state.update(event.pop('state_updates',{}));state['updated_utc']=utc;state['record_owner']='/root/launch_coordination';state['last_operation']=event['operation']
statepath.write_text(json.dumps(state,ensure_ascii=False,indent=2)+'\n')
with (own/'events.jsonl').open('a') as f:f.write(json.dumps(event,ensure_ascii=False)+'\n');f.flush();os.fsync(f.fileno())
body=event['body'].replace('[[UTC]]',utc)
hour=own/'logs'/now.strftime('%Y-%m-%d')/(now.strftime('%H')+'.md');hour.parent.mkdir(parents=True,exist_ok=True)
with hour.open('a') as f:f.write('\n\n'+body+'\n');f.flush();os.fsync(f.fileno())
if event.get('shared_append',False):
 raise RuntimeError('AGENT.md append disabled by latest explicit user instruction')
if False:
 with ExitStack() as stack:
  for name in ('.AGENT.md.lock','AGENT.md.lock'):
   f=stack.enter_context((shared.parent/name).open('a'));fcntl.flock(f.fileno(),fcntl.LOCK_EX)
  dest=stack.enter_context(shared.open('a'));fcntl.flock(dest.fileno(),fcntl.LOCK_EX)
  old=shared.read_bytes();payload=('\n\n'+body+'\n').encode();count=os.write(dest.fileno(),payload)
  if count!=len(payload):raise RuntimeError('Incomplete shared append')
  os.fsync(dest.fileno())
 print(json.dumps({'utc':utc,'shared_append_bytes':count,'prior_shared_bytes':len(old),'state':str(statepath)}))
else:print(json.dumps({'utc':utc,'shared_append':False,'state':str(statepath)}))
