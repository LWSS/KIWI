#!/usr/bin/env python3
"""Run retail/candidate CoD4Rad on isolated BSP copies and compare results."""
from __future__ import annotations
import argparse, hashlib, json, os, shutil, struct, subprocess, tempfile
from pathlib import Path
def parse():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('fixture',type=Path,help='input .d3dbsp (original is never written)');p.add_argument('--game-root',type=Path,required=True,help='retail CoD4 root; data trees are linked, never copied');p.add_argument('--retail',type=Path,required=True);p.add_argument('--candidate',type=Path,required=True);p.add_argument('--arg',action='append',default=[],help='one common cod4rad option; repeat');p.add_argument('--timeout',type=int,default=1800);p.add_argument('--keep',action='store_true');p.add_argument('--report',type=Path);return p.parse_args()
def sha(p):
 h=hashlib.sha256()
 with p.open('rb') as f:
  for b in iter(lambda:f.read(1<<20),b''):h.update(b)
 return h.hexdigest()
def link_data(root,game):
 root.mkdir(parents=True,exist_ok=True)
 for name in ('main','raw','raw_shared','devraw','devraw_shared','main_shared'):
  source=game/name
  if not source.is_dir():continue
  dest=root/name
  try:os.symlink(source,dest,target_is_directory=True)
  except OSError:subprocess.run(['cmd.exe','/c','mklink','/J',str(dest),str(source)],check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
def stage(root,fixture,game):
 link_data(root,game)
 maps=root/'maps';maps.mkdir(parents=True);target=maps/fixture.name;shutil.copy2(fixture,target)
 for ext in ('.d3dprt','.d3dpoly'):
  side=fixture.with_suffix(ext)
  if side.is_file():shutil.copy2(side,maps/side.name)
 return target
def run(exe,root,maparg,args,timeout):
 try:
  r=subprocess.run([str(exe),*args,str((root/maparg).resolve())],cwd=root,text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=timeout);return {'exit':r.returncode,'output':r.stdout}
 except subprocess.TimeoutExpired as e:return {'exit':None,'output':e.stdout if isinstance(e.stdout,str) else '','timeout':True}
def lumps(path):
 data=path.read_bytes();out={'size':len(data),'sha256':sha(path),'lumps':[]}
 if len(data)<12:return out
 magic,version,count=struct.unpack_from('<4sII',data);out.update(magic=magic.decode('latin1'),version=version,chunk_count=count)
 if count>100 or 12+8*count>len(data):return out
 off=12+8*count
 for i in range(count):
  lump_type,n=struct.unpack_from('<II',data,12+i*8)
  item={'index':lump_type,'chunk':i,'offset':off,'length':n}
  if off+n<=len(data):item['sha256']=hashlib.sha256(data[off:off+n]).hexdigest()
  else:item['invalid']=True
  out['lumps'].append(item);off+=(n+3)&~3
 return out
def compare(a,b):
 al={x['index']:x for x in a['lumps']};bl={x['index']:x for x in b['lumps']};return {'identical_bytes':a['sha256']==b['sha256'],'size_delta':b['size']-a['size'],'different_lumps':[i for i in sorted(set(al)|set(bl)) if al.get(i,{}).get('length')!=bl.get(i,{}).get('length') or al.get(i,{}).get('sha256')!=bl.get(i,{}).get('sha256')]}
def keep_outputs(base):
 kept=Path(tempfile.mkdtemp(prefix='cod4rad_diff_kept_'))
 # The data trees under each staged root are symlinks or NTFS junctions into
 # the game install.  Copy only the writable maps trees; following a junction
 # here would duplicate the entire game and defeat the isolation guarantee.
 for side in ('retail','candidate'):
  source=base/side/'maps';dest=kept/side/'maps'
  if source.is_dir():shutil.copytree(source,dest)
 return kept
def main():
 a=parse();fixture=a.fixture.resolve()
 if fixture.suffix.lower()!='.d3dbsp' or not fixture.is_file():raise SystemExit('fixture must be an existing .d3dbsp')
 game=a.game_root.resolve()
 if not game.is_dir():raise SystemExit(f'game root not found: {game}')
 for e in (a.retail,a.candidate):
  if not e.is_file():raise SystemExit(f'executable not found: {e}')
 with tempfile.TemporaryDirectory(prefix='cod4rad_diff_') as td:
  base=Path(td);rr=base/'retail';cr=base/'candidate';rt=stage(rr,fixture,game);ct=stage(cr,fixture,game);maparg='maps/'+fixture.stem
  report={'fixture':str(fixture),'args':a.arg,'retail':run(a.retail,rr,maparg,a.arg,a.timeout),'candidate':run(a.candidate,cr,maparg,a.arg,a.timeout),'temporary_root':str(base),'retail_bsp':lumps(rt),'candidate_bsp':lumps(ct)};report['comparison']=compare(report['retail_bsp'],report['candidate_bsp'])
  kept=keep_outputs(base) if a.keep else None
  if kept:report['kept_root']=str(kept)
  print(json.dumps(report,indent=2))
  if a.report:a.report.write_text(json.dumps(report,indent=2)+'\n',encoding='utf8')
  if kept:print(f'kept staged outputs: {kept}')
if __name__=='__main__':main()
