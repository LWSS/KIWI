#!/usr/bin/env python3
"""Apply coherent native source-path clusters as CoD4Rad TU anchors."""
from __future__ import annotations
import argparse,csv,json,ntpath,re
from pathlib import Path

GAP=0x10000
ACOLS='address ida_name selected_tu candidate_tus anchor_class raw_paths'.split()
MCOLS='cod4_tu anchor_addresses address_start address_end confidence evidence status'.split()
def parse():
 p=argparse.ArgumentParser();p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3]);return p.parse_args()
def canon(s):
 s=s.strip().replace('/','\\').lower()
 if not re.search(r'\.(c|cpp)$',s) or 'vctools\\crt_bld' in s or s.startswith('i386\\'):return None
 if 'cod3src\\' in s:s=s.split('cod3src\\',1)[1]
 s=re.sub(r'^(?:\.\\|\.\.\\|src\\)+','',s).replace('src\\universal\\..\\','')
 return ntpath.normpath(s)
def main():
 repo=parse().repo.resolve(); fun=json.loads((repo/'IDA'/'cod4rad_functions.json').read_text(encoding='utf8')); anchors=[]
 for f in fun:
  paths=sorted({x.strip() for x in f.get('strings',[]) if re.search(r'(?i)\.(?:c|cpp|h)$',x.strip())});tus=sorted({x for x in map(canon,paths) if x})
  if tus:anchors.append(dict(address=f['address'],ida_name=f['ida_name'],selected_tu=tus[0] if len(tus)==1 else '',candidate_tus=' | '.join(tus),anchor_class='AMBIGUOUS' if len(tus)>1 else '',raw_paths=' | '.join(paths)))
 # A source path copied by an inline assert is retained but cannot claim the
 # remote TU.  For each TU only its largest coherent code-address cluster wins.
 by_tu={}
 for a in anchors:
  if a['selected_tu']:by_tu.setdefault(a['selected_tu'],[]).append(a)
 eligible={}
 for tu,items in by_tu.items():
  items.sort(key=lambda a:int(a['address'],16));clusters=[]
  for a in items:
   if not clusters or int(a['address'],16)-int(clusters[-1][-1]['address'],16)>GAP:clusters.append([])
   clusters[-1].append(a)
  dominant=max(clusters,key=lambda c:(len(c),-int(c[0]['address'],16)));selected={a['address'] for a in dominant}
  for a in items:
   if a['address'] in selected:
    a['anchor_class']='DOMINANT_SOURCE_CLUSTER' if len(dominant)>1 else 'SINGLE_SOURCE_REF';eligible[a['address']]=a
   else:a['anchor_class']='INLINE_PATH_OUTLIER'
 with (repo/'COD4RAD_SOURCE_PATH_ANCHORS.csv').open('w',newline='',encoding='utf8') as f:w=csv.DictWriter(f,fieldnames=ACOLS);w.writeheader();w.writerows(anchors)
 ledger=repo/'COD4RAD_FUNCTION_LEDGER.csv';applied=upgraded=0
 if ledger.exists():
  with ledger.open(newline='',encoding='utf8') as f:rows=list(csv.DictReader(f));cols=list(rows[0]) if rows else []
  for r in rows:
   a=eligible.get(r['address'])
   if not a:continue
   confidence='EXACT' if a['anchor_class']=='DOMINANT_SOURCE_CLUSTER' else 'HIGH'; evidence=f"{a['anchor_class'].lower()}: {a['raw_paths'].split(' | ')[0]}"
   # Never displace a conflicting/manual TU.  A prior run's native-path HIGH
   # is safe to refine to EXACT once the dominant cluster confirms it.
   if not r.get('cod4_tu'):
    r['cod4_tu']=a['selected_tu'];r['tu_confidence']=confidence;applied+=1
   elif r['cod4_tu']==a['selected_tu'] and confidence=='EXACT' and r.get('tu_confidence')!='EXACT' and ('native source path:' in r.get('evidence','') or 'source_cluster' in r.get('evidence','')):
    r['tu_confidence']='EXACT';upgraded+=1
   else:continue
   if evidence not in r.get('evidence',''):r['evidence']=(r.get('evidence','')+'; ' if r.get('evidence') else '')+evidence
  with ledger.open('w',newline='',encoding='utf8') as f:w=csv.DictWriter(f,fieldnames=cols);w.writeheader();w.writerows(rows)
 manifest=repo/'COD4RAD_TU_MANIFEST.csv'
 if manifest.exists():
  with manifest.open(newline='',encoding='utf8') as f:rows=list(csv.DictReader(f));cols=list(rows[0]) if rows else MCOLS
 else:rows=[];cols=MCOLS
 existing={r.get('cod4_tu'):r for r in rows}
 grouped={}
 for a in eligible.values():grouped.setdefault(a['selected_tu'],[]).append(a)
 for tu,items in grouped.items():
  addrs=sorted((a['address'] for a in items),key=lambda x:int(x,16));conf='EXACT' if len(items)>1 else 'HIGH'; evidence=f'{len(items)} ownership-eligible source-path anchors'
  row=existing.get(tu)
  if row is None:row=dict(cod4_tu=tu);rows.append(row)
  if not row.get('anchor_addresses') or row.get('evidence','').endswith('source-path references'):
   row.update(anchor_addresses=' | '.join(addrs),address_start=addrs[0],address_end=addrs[-1],confidence=conf,evidence=evidence,status='TU_ANCHORED')
 with manifest.open('w',newline='',encoding='utf8') as f:w=csv.DictWriter(f,fieldnames=cols);w.writeheader();w.writerows(rows)
 out=sum(a['anchor_class']=='INLINE_PATH_OUTLIER' for a in anchors)
 print(f'source-path rows: {len(anchors)}; ownership-eligible: {len(eligible)}; TUs: {len(grouped)}; inline outliers: {out}')
 print(f'ledger assignments: {applied}; prior native-path assignments upgraded to EXACT: {upgraded}')
if __name__=='__main__':main()
