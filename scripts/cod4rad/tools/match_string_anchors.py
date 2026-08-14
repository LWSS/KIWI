#!/usr/bin/env python3
"""Match uniquely owned local-source literals to CoD4Rad IDA functions.

This is deliberately a one-way, empty-fields-only pass: reviewed names, TU
assignments, and evidence are never replaced.
"""
from __future__ import annotations
import argparse, collections, csv, json, re
from pathlib import Path

STRING=re.compile(r'"(?:\\.|[^"\\])*"')
CONTROL_WORDS={"if","for","while","switch","catch"}
def parse():
 p=argparse.ArgumentParser();p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3]);return p.parse_args()
def unquote(x):
 try:return bytes(x[1:-1],'utf8').decode('unicode_escape')
 except UnicodeDecodeError:return x[1:-1]
def useful(x): return len(x.strip())>=5 and not re.match(r'^[\s\d.,:/\\-]+$',x)
def mask_comments(text):
 out=list(text); i=0; state='code'
 while i<len(text):
  c=text[i]; n=text[i+1] if i+1<len(text) else ''
  if state=='code':
   if c=='/' and n=='/': out[i]=out[i+1]=' '; i+=2; state='line'; continue
   if c=='/' and n=='*': out[i]=out[i+1]=' '; i+=2; state='block'; continue
   if c=='"': state='string'
   elif c=="'": state='char'
  elif state=='line':
   if c=='\n': state='code'
   else: out[i]=' '
  elif state=='block':
   if c=='*' and n=='/': out[i]=out[i+1]=' '; i+=2; state='code'; continue
   if c!='\n': out[i]=' '
  elif state in {'string','char'}:
   quote='"' if state=='string' else "'"
   if c=='\\': i+=2; continue
   if c==quote: state='code'
  i+=1
 return ''.join(out)
def top_level_functions(text):
 """Yield (name, body) for braces opened at C/C++ file scope.

 Looking only at file-scope braces prevents control statements inside a
 function from being mistaken for definitions.  The previous regex-only pass
 could incorrectly call a function ``if`` or an identifier from a condition.
 """
 clean=mask_comments(text); depth=0; state='code'; opens=[]; i=0
 while i<len(clean):
  c=clean[i]
  if state=='code':
   if c=='"': state='string'
   elif c=="'": state='char'
   elif c=='{':
    if depth==0: opens.append(i)
    depth+=1
   elif c=='}' and depth: depth-=1
  elif state in {'string','char'}:
   quote='"' if state=='string' else "'"
   if c=='\\': i+=2; continue
   if c==quote: state='code'
  i+=1
 for start in opens:
  end_sig=start-1
  while end_sig>=0 and clean[end_sig].isspace(): end_sig-=1
  if end_sig<0 or clean[end_sig]!=')': continue
  paren=1; j=end_sig-1
  while j>=0 and paren:
   if clean[j]==')': paren+=1
   elif clean[j]=='(': paren-=1
   j-=1
  if paren: continue
  k=j
  while k>=0 and clean[k].isspace(): k-=1
  name_end=k+1
  while k>=0 and (clean[k].isalnum() or clean[k]=='_'): k-=1
  name=clean[k+1:name_end]
  if not name or name in CONTROL_WORDS: continue
  # Reject assignments and typedef/aggregate declarations masquerading as a
  # signature.  Real definitions have a return-type/declarator before name.
  sig_start=max(clean.rfind(';',0,k+1),clean.rfind('}',0,k+1))+1
  prefix=clean[sig_start:k+1].strip()
  if not prefix or '=' in prefix or re.search(r'\btypedef\b',prefix): continue
  depth=1; state='code'; q=start+1
  while q<len(clean) and depth:
   c=clean[q]
   if state=='code':
    if c=='"': state='string'
    elif c=="'": state='char'
    elif c=='{': depth+=1
    elif c=='}': depth-=1
   elif state in {'string','char'}:
    quote='"' if state=='string' else "'"
    if c=='\\': q+=2; continue
    if c==quote: state='code'
   q+=1
  if depth==0: yield name,clean[start+1:q-1]
def source_functions(root):
 owners=collections.defaultdict(list); tus=collections.defaultdict(set)
 for path in sorted(root.glob('*')):
  if path.suffix.lower() not in {'.c','.cpp'}:continue
  text=path.read_text(encoding='utf8',errors='ignore')
  for name,body in top_level_functions(text):
   for literal in set(map(unquote,STRING.findall(body))):
    if useful(literal): owners[literal].append((path.name,name));tus[literal].add(path.name)
 return owners,tus
def main():
 repo=parse().repo.resolve(); ida=json.loads((repo/'IDA'/'cod4rad_functions.json').read_text(encoding='utf8')); src=repo/'src'/'cod4rad'; owners,tus=source_functions(src)
 freq=collections.Counter(x for f in ida for x in set(f.get('strings',[])))
 candidates=[]
 for f in ida:
  hits=[]
  for s in set(f.get('strings',[])):
   # A literal must identify one source TU/function and one IDA function.
   if s in owners and len(tus[s])==1 and freq[s]==1:
    hit=owners[s]
    if len(set(hit))==1:hits.append((s,*hit[0]))
  pairs=collections.Counter((tu,name) for _,tu,name in hits)
  if len(pairs)==1:
   (tu,name),count=pairs.popitem()
   candidates.append(dict(address=f['address'],ida_name=f['ida_name'],cod4_tu=tu,candidate_name=name,confidence='HIGH',evidence_strings=[x[0] for x in hits],hit_count=count))
 (repo/'IDA'/'cod4rad_string_anchor_candidates.json').write_text(json.dumps(candidates,indent=2)+'\n',encoding='utf8')
 ledger=repo/'COD4RAD_FUNCTION_LEDGER.csv'
 with ledger.open(newline='',encoding='utf8') as f:rows=list(csv.DictReader(f));cols=list(rows[0]) if rows else []
 by={x['address']:x for x in candidates}; applied=0
 for r in rows:
  c=by.get(r['address'])
  if not c:continue
  if not r.get('cod4_tu'):r['cod4_tu']=c['cod4_tu'];r['tu_confidence']=c['confidence'];applied+=1
  if not r.get('final_name'):r['final_name']=c['candidate_name'];applied+=1
  if not r.get('evidence'):r['evidence']='unique local-source literal: '+c['evidence_strings'][0]
 with ledger.open('w',newline='',encoding='utf8') as f:w=csv.DictWriter(f,fieldnames=cols);w.writeheader();w.writerows(rows)
 print(f'unique source/IDA candidates: {len(candidates)}; empty fields populated: {applied}')
if __name__=='__main__':main()
