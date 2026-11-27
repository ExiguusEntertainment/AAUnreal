"""Read-only reproduction of 3D_VIEW.C5675 IFindSectorNum for L30 Thing17.
Run with ordinary Python from the project root; no Unreal/content writes.
"""
from pathlib import Path
import struct
b=Path('Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L30.MAP').read_bytes();_,n,off=struct.unpack_from('<4sII',b);lumps={}
for i in range(n):
 at,size,name=struct.unpack_from('<II8s',b,off+i*16);lumps[name.rstrip(b'\0').decode()]=b[at:at+size]
v=list(struct.iter_unpack('<hh',lumps['VERTEXES']));l=list(struct.iter_unpack('<HHHHHHH',lumps['LINEDEFS']));s=list(struct.iter_unpack('<hh8s8s8sH',lumps['SIDEDEFS']));bm=list(struct.unpack('<'+'H'*(len(lumps['BLOCKMAP'])//2),lumps['BLOCKMAP']));ox,oy,cols,rows=struct.unpack_from('<hhHH',lumps['BLOCKMAP']);x,y=2360,3188;c=(x-ox)>>7;r=(y-oy)>>7
print('BLOCKMAP',ox,oy,cols,rows,'point',x,y,'cell',c,r)
def side(x,y,line):
 a,b=v[l[line][0]],v[l[line][1]];calc=(a[1]-b[1])*(x-b[0])-(a[0]-b[0])*(y-b[1]);return 0 if calc<0 else 1 if calc>0 else 2
found=65534;last=65535;lastside=0;history=[]
while found==65534 and 0<=c<cols and 0<=r<rows:
 at=1+bm[4+r*cols+c];print('row',r,'offset',at);lines=[]
 while bm[at]!=65535:
  line=bm[at];at+=1;lines.append(line);si=side(x,y,line)
  if si==2:si=0 if found==65534 else 1-lastside
  new=False
  if l[line][5+si]!=65535:
   if found!=65534:
    new=side(*v[l[line][0]],last)==lastside or side(*v[l[line][1]],last)==lastside
   else:new=True
  for old,oldside in history:
   if side(*v[l[line][0]],old)!=oldside and side(*v[l[line][1]],old)!=oldside:new=False;break
  if new:
   if last!=65535:history.append((last,lastside))
   found=l[line][5+si];last=line;lastside=si;print('candidate',line,'side',si,'sidedef',found,'sector',s[found][-1])
 print('checked lines',lines)
 if found==65534:r+=1
print('source lookup',s[found][-1] if found not in(65534,65535) else 65535,'setup fallback if invalid=0')

assert s[found][-1] == 120, "Original BLOCKMAP mounting result changed"
