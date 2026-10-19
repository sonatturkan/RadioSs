import tarfile,sys,re,io,subprocess,json
db=sys.argv[1]; roots=sys.argv[2:]
raw=subprocess.run(['zstd','-dc',db],capture_output=True,check=True).stdout
t=tarfile.open(fileobj=io.BytesIO(raw))
pk={}; prov={}
for m in t.getmembers():
    if not m.name.endswith('/desc'): continue
    txt=t.extractfile(m).read().decode()
    f={}; cur=None
    for line in txt.split('\n'):
        if line.startswith('%') and line.endswith('%'): cur=line.strip('%'); f[cur]=[]
        elif line and cur: f[cur].append(line)
    name=f['NAME'][0]; pk[name]=f
    for p in f.get('PROVIDES',[]): prov[re.split('[<>=]',p)[0]]=name
def res(d):
    n=re.split('[<>=]',d)[0]
    return n if n in pk else prov.get(n)
seen=set(); order=[]; st=list(roots)
while st:
    r=st.pop(); n=res(r)
    if not n: print('MISSING',r,file=sys.stderr); continue
    if n in seen: continue
    seen.add(n); order.append(n)
    st.extend(pk[n].get('DEPENDS',[]))
tot=0
for n in sorted(order):
    f=pk[n]; tot+=int(f['CSIZE'][0]); print(f['FILENAME'][0], f['SHA256SUM'][0], f['VERSION'][0])
print('TOTAL_MB',tot/1e6,file=sys.stderr)
