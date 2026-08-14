#!/usr/bin/env python3
"""Fill empty TU fields only inside address spans bracketed by exact anchors."""
from __future__ import annotations
import argparse,csv
from pathlib import Path
def parse():
 p=argparse.ArgumentParser();p.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[3]);return p.parse_args()
def main():
 ledger=parse().repo.resolve()/'COD4RAD_FUNCTION_LEDGER.csv'
 with ledger.open(newline='',encoding='utf8') as f:rows=list(csv.DictReader(f));cols=list(rows[0]) if rows else []
 # Only explicit EXACT assignments qualify; HIGH source paths remain visible
 # but cannot spread through potentially inlined assertion islands.
 exact=[(i,r) for i,r in enumerate(rows) if r.get('tu_confidence')=='EXACT' and r.get('cod4_tu')]
 applied=0
 for (li,left),(ri,right) in zip(exact,exact[1:]):
  if left['cod4_tu']!=right['cod4_tu']:continue
  for r in rows[li+1:ri]:
   if r.get('cod4_tu'):continue
   r['cod4_tu']=left['cod4_tu'];r['tu_confidence']='HIGH';e=f"bracketed by exact {left['cod4_tu']} anchors {left['address']} and {right['address']}"
   if not r.get('evidence'):r['evidence']=e
   applied+=1
 with ledger.open('w',newline='',encoding='utf8') as f:w=csv.DictWriter(f,fieldnames=cols);w.writeheader();w.writerows(rows)
 pairs=sum(1 for (_,a),(_,b) in zip(exact,exact[1:]) if a['cod4_tu']==b['cod4_tu'])
 print(f'empty TU assignments propagated: {applied}; exact anchor pairs: {pairs}')
if __name__=='__main__':main()
