#!/usr/bin/env python3
"""Export the live CoD4Rad IDA database into reproducible local ledgers."""
from __future__ import annotations

import argparse, asyncio, csv, hashlib, json
from pathlib import Path
from typing import Any
from mcp import ClientSession
from mcp.client.streamable_http import streamablehttp_client

ENDPOINT = "http://127.0.0.1:13338/mcp"
IDB = Path(r"F:\SteamLibrary\steamapps\common\Call of Duty 4\bin\cod4rad.exe.i64")
FCOLS = "address end size ida_name final_name library_status flags prototype caller_count callee_count basic_block_count string_refs cod2_candidate cod4_tu tu_confidence change_class implementation evidence test review_status".split()
GCOLS = "address end size ida_name final_name segment flags prototype xref_count xref_functions cod4_tu donor_symbol change_class implementation evidence review_status".split()

def args():
 p=argparse.ArgumentParser(); p.add_argument('--endpoint',default=ENDPOINT); p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3]); p.add_argument('--chunk-size',type=int,default=100); p.add_argument('--idb',type=Path,default=IDB); return p.parse_args()
def digest(p:Path):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''): h.update(b)
 return h.hexdigest()
def fcode(off,count,out): return f'''import ida_bytes,ida_funcs,ida_gdl,ida_nalt,idautils,idc,json
rows=[]; eas=list(idautils.Functions())
for ea in eas[{off}:{off+count}]:
 fn=ida_funcs.get_func(ea); callers=set(); callees=set(); strings=set()
 for x in idautils.CodeRefsTo(ea,False):
  owner=ida_funcs.get_func(x)
  if owner: callers.add(owner.start_ea)
 for item in idautils.FuncItems(ea):
  for target in idautils.CodeRefsFrom(item,False):
   callee=ida_funcs.get_func(target)
   if callee and callee.start_ea!=ea: callees.add(callee.start_ea)
  for target in idautils.DataRefsFrom(item):
   s=ida_bytes.get_strlit_contents(target,-1,ida_nalt.STRTYPE_C)
   if s: strings.add(s.decode('utf-8',errors='replace'))
 try: blocks=sum(1 for _ in ida_gdl.FlowChart(fn))
 except: blocks=0
 rows.append(dict(address=hex(fn.start_ea),end=hex(fn.end_ea),size=fn.end_ea-fn.start_ea,ida_name=ida_funcs.get_func_name(fn.start_ea),flags=fn.flags,is_library=bool(fn.flags&ida_funcs.FUNC_LIB),is_thunk=bool(fn.flags&ida_funcs.FUNC_THUNK),prototype=idc.get_type(fn.start_ea),callers=[hex(x) for x in sorted(callers)],callees=[hex(x) for x in sorted(callees)],basic_block_count=blocks,strings=sorted(strings)))
json.dump(rows,open({str(out)!r},'w',encoding='utf8')); result=json.dumps(dict(total=len(eas),count=len(rows)))'''
def gcode(out): return f'''import ida_bytes,ida_funcs,ida_segment,idautils,idc,json
c={{ea:n for ea,n in idautils.Names()}}
for f in idautils.Functions():
 for i in idautils.FuncItems(f):
  for ea in idautils.DataRefsFrom(i): c.setdefault(ea,idc.get_name(ea,idc.GN_VISIBLE) or 'data_%X'%ea)
rows=[]
for ea,name in sorted(c.items()):
 seg=ida_segment.getseg(ea)
 if not seg or ida_segment.get_segm_name(seg) not in ('.rdata','.data') or ida_bytes.is_strlit(ida_bytes.get_full_flags(ea)): continue
 xs=list(idautils.XrefsTo(ea,0)); owners=set()
 for x in xs:
  f=ida_funcs.get_func(x.frm)
  if f: owners.add(idc.get_func_name(f.start_ea) or hex(f.start_ea))
 owners=sorted(owners)
 rows.append(dict(address=hex(ea),end=hex(ea+max(ida_bytes.get_item_size(ea),1)),size=max(ida_bytes.get_item_size(ea),1),ida_name=name,segment=ida_segment.get_segm_name(seg),flags=hex(ida_bytes.get_full_flags(ea)),prototype=idc.get_type(ea) or '',xref_count=len(xs),xref_functions=owners))
json.dump(rows,open({str(out)!r},'w',encoding='utf8')); result=json.dumps(dict(count=len(rows)))'''
async def call(s,t,a):
 r=await s.call_tool(t,a)
 if r.isError: raise RuntimeError(f'{t}: {r.content}')
 return r.structuredContent
def prior(path):
 if not path.exists(): return {}
 with path.open(newline='',encoding='utf8') as f:return {r['address']:r for r in csv.DictReader(f)}
def write_ledger(path,cols,items,generated):
 old=prior(path)
 with path.open('w',newline='',encoding='utf8') as f:
  w=csv.DictWriter(f,fieldnames=cols); w.writeheader()
  for item in items:
   r={c:'' for c in cols}; r.update(old.get(item['address'],{})); r.update({c:(' | '.join(v) if isinstance(v,list) else v) for c,v in item.items() if c in generated})
   r['review_status']=r['review_status'] or 'INVENTORIED'; w.writerow(r)
async def run(a):
 repo=a.repo.resolve(); raw=repo/'IDA'; raw.mkdir(exist_ok=True); chunk=raw/'cod4rad_function_chunk.json'
 async with streamablehttp_client(a.endpoint) as (rd,wr,_):
  async with ClientSession(rd,wr) as s:
   await s.initialize(); health=await call(s,'server_health',{})
   if health.get('module','').lower()!='cod4rad.exe': raise RuntimeError(f'wrong IDA database: {health}')
   survey=await call(s,'survey_binary',{'detail_level':'minimal'}); fun=[]; off=0; total=None
   while total is None or off<total:
    v=await call(s,'py_eval',{'code':fcode(off,a.chunk_size,chunk)}); info=json.loads(v['result']); rows=json.loads(chunk.read_text(encoding='utf8')); total=int(info['total']); fun+=rows; off+=len(rows); print(f'functions: {off}/{total}')
    if not rows: break
   if len(fun)!=total: raise RuntimeError(f'incomplete functions: {len(fun)}/{total}')
   chunk.unlink(missing_ok=True); gp=raw/'cod4rad_global_inventory.json'; v=await call(s,'py_eval',{'code':gcode(gp)}); glob=json.loads(gp.read_text(encoding='utf8'))
   imp=await call(s,'entity_query',{'queries':{'kind':'imports','count':0}}); st=await call(s,'entity_query',{'queries':{'kind':'strings','count':0}})
 (raw/'cod4rad_metadata.json').write_text(json.dumps({'health':health,'survey':survey,'idb_path':str(a.idb),'idb_sha256':digest(a.idb)},indent=2)+'\n',encoding='utf8')
 for name,data in [('functions',fun),('globals',glob),('imports',imp),('strings',st)]: (raw/f'cod4rad_{name}.json').write_text(json.dumps(data,indent=2)+'\n',encoding='utf8')
 for x in fun: x.update(library_status='LIBRARY' if x['is_library'] else ('THUNK' if x['is_thunk'] else 'COMPILER'),change_class='THIRD_PARTY' if x['is_library'] else ('THUNK_OR_GLUE' if x['is_thunk'] else ''),final_name='' if x['ida_name'].startswith('sub_') else x['ida_name'])
 write_ledger(repo/'COD4RAD_FUNCTION_LEDGER.csv',FCOLS,fun,set(FCOLS)-{'cod2_candidate','cod4_tu','tu_confidence','implementation','evidence','test','review_status'})
 for x in glob: x['final_name']='' if x['ida_name'].startswith(('byte_','word_','dword_','qword_','flt_','dbl_','unk_','off_')) else x['ida_name']
 write_ledger(repo/'COD4RAD_GLOBAL_LEDGER.csv',GCOLS,glob,{'address','end','size','ida_name','segment','flags','prototype','xref_count','xref_functions','final_name'})
 print(f'wrote inventories and ledgers under {repo}')
def main():
 a=args()
 if a.chunk_size<=0: raise SystemExit('--chunk-size must be positive')
 if not a.idb.is_file(): raise SystemExit(f'IDB not found: {a.idb}')
 asyncio.run(run(a))
if __name__=='__main__': main()
