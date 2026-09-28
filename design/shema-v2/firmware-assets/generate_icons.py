"""Export the Shema icon family: rounded SVGs and tinted LVGL A8 masks.

Standard library only. The paths here are the source of truth. Playback controls
use filled silhouettes; navigation and content icons share a 2-unit round stroke.
"""
import re, math, json, hashlib
from pathlib import Path
P = Path(__file__).resolve().parent
# Generated C sources go straight into the firmware's UI component.
UI = P.parents[2]/'audio_bible'/'components'/'app_ui'


def gear_path():
    # Six broad, rounded teeth stay readable on a 24px display icon.
    points = []
    for tooth in range(6):
        for angle, radius in ((-23, 7.5), (-12, 10), (12, 10), (23, 7.5)):
            a = math.radians(tooth * 60 + angle - 90)
            points.append((12 + radius * math.cos(a), 12 + radius * math.sin(a)))
    def toward(a, b, distance=.55):
        length = math.dist(a, b)
        return tuple(x + (y - x) * min(distance / length, .4) for x, y in zip(a, b))
    parts = []
    for i, point in enumerate(points):
        before = toward(point, points[i - 1])
        after = toward(point, points[(i + 1) % len(points)])
        parts.append(f'{"M" if i == 0 else "L"}{before[0]:.3f} {before[1]:.3f} Q{point[0]:.3f} {point[1]:.3f} {after[0]:.3f} {after[1]:.3f}')
    return ' '.join(parts) + ' Z M15 12 A3 3 0 1 1 9 12 A3 3 0 1 1 15 12 Z'


paths = {
    'book': 'M12 6 C9 3.7 5.8 3.6 3 4.7 V19 C6 17.8 9.3 18.2 12 20 C14.7 18.2 18 17.8 21 19 V4.7 C18.2 3.6 15 3.7 12 6 Z M12 6 V20',
    'year': 'M7 5 H5 Q3 5 3 7 V19 Q3 21 5 21 H19 Q21 21 21 19 V7 Q21 5 19 5 H17 M7 3 V7 M17 3 V7 M7 5 H17 M3 10 H21 M8 15 L11 18 L16 13',
    'folder': 'M3 8 V6 Q3 4 5 4 H9 Q10 4 11 5 L13 7 H19 Q21 7 21 9 V18 Q21 20 19 20 H5 Q3 20 3 18 Z M3 9 H21',
    'heart': 'M12 20 C10 18.4 3 13.4 3 8.3 C3 3.5 9 2.5 12 7 C15 2.5 21 3.5 21 8.3 C21 13.4 14 18.4 12 20 Z',
    'back': 'M14.5 5.5 L8 12 L14.5 18.5',
    'next': 'M9.5 5.5 L16 12 L9.5 18.5',
    'play': 'M8.7 4.9 Q7.5 4.2 7.5 5.7 V18.3 Q7.5 19.8 8.7 19.1 L19.4 12.8 Q20.7 12 19.4 11.2 Z',
    'pause': 'M7 5 H9 Q10 5 10 6 V18 Q10 19 9 19 H7 Q6 19 6 18 V6 Q6 5 7 5 Z M15 5 H17 Q18 5 18 6 V18 Q18 19 17 19 H15 Q14 19 14 18 V6 Q14 5 15 5 Z',
    'prev': 'M5 5 H6 Q7 5 7 6 V18 Q7 19 6 19 H5 Q4 19 4 18 V6 Q4 5 5 5 Z M18.8 5.2 Q20 4.5 20 6 V18 Q20 19.5 18.8 18.8 L9 12.8 Q7.8 12 9 11.2 Z',
    'skip': 'M18 5 H19 Q20 5 20 6 V18 Q20 19 19 19 H18 Q17 19 17 18 V6 Q17 5 18 5 Z M5.2 5.2 Q4 4.5 4 6 V18 Q4 19.5 5.2 18.8 L15 12.8 Q16.2 12 15 11.2 Z',
    'more': 'M6 12 A1.5 1.5 0 1 1 3 12 A1.5 1.5 0 1 1 6 12 Z M13.5 12 A1.5 1.5 0 1 1 10.5 12 A1.5 1.5 0 1 1 13.5 12 Z M21 12 A1.5 1.5 0 1 1 18 12 A1.5 1.5 0 1 1 21 12 Z',
    'recent': 'M3.5 8.5 A8.5 8.5 0 1 1 3.5 15.5 M3.5 3.5 V8.5 H8.5 M12 7 V12 L15.5 14',
    'settings': gear_path(),
    'moon': 'M20.5 14 A8.6 8.6 0 1 1 10 3.5 A7 7 0 0 0 20.5 14 Z',
    'volume': 'M4 9 H7 L12 5 V19 L7 15 H4 Q3 15 3 14 V10 Q3 9 4 9 Z M16 9 Q19 12 16 15 M19 6 Q24 12 19 18',
    'minus': 'M5 12 H19',
    'plus': 'M5 12 H19 M12 5 V19',
    'shuffle': 'M3 6 H5 Q7 6 9 9 L15 17 Q16 18 18 18 H21 M17 14 L21 18 L17 22 M3 18 H5 Q7 18 9 15 M15 7 Q16 6 18 6 H21 M17 2 L21 6 L17 10',
    'check': 'M5 12 L10 17 L19 7',
    'sun': 'M12 2.5 V4 M12 20 V21.5 M2.5 12 H4 M20 12 H21.5 M5.3 5.3 L6.5 6.5 M17.5 17.5 L18.7 18.7 M5.3 18.7 L6.5 17.5 M17.5 6.5 L18.7 5.3 M16 12 A4 4 0 1 1 8 12 A4 4 0 1 1 16 12 Z',
    'power': 'M12 3 V11 M7 5.5 A8.5 8.5 0 1 0 17 5.5',
    'trash': 'M4 6 H20 M9 6 V4 Q9 3 10 3 H14 Q15 3 15 4 V6 M6 6 L7 19 Q7 21 9 21 H15 Q17 21 17 19 L18 6 M10 10 V17 M14 10 V17',
    'sd': 'M7 3 H15 L20 8 V19 Q20 21 18 21 H7 Q5 21 5 19 V5 Q5 3 7 3 Z M9 6 V10 M12 6 V10 M15 7 V10',
    'battery': 'M4 7 H18 Q20 7 20 9 V15 Q20 17 18 17 H4 Q2 17 2 15 V9 Q2 7 4 7 Z M22 10 V14 M6 10 V14 M10 10 V14 M14 10 V14',
    'warning': 'M10.7 4 Q12 2 13.3 4 L21 18 Q22.4 21 19 21 H5 Q1.6 21 3 18 Z M12 9 V14 M12 17 V17.2',
    'home': 'M3 11 L12 3 L21 11 M5 10 V19 Q5 21 7 21 H10 V15 H14 V21 H17 Q19 21 19 19 V10',
    'audio': 'M9 17 V5 L20 3 V15 M9 8 L20 6 M9 17 C9 13 3 14 3 18 C3 22 9 21 9 17 Z M20 15 C20 11 14 12 14 16 C14 20 20 19 20 15 Z',
    'list': 'M9 6 H20 M9 12 H20 M9 18 H20 M4 6 H4.2 M4 12 H4.2 M4 18 H4.2',
    'up': 'M5.5 14.5 L12 8 L18.5 14.5',
    'down': 'M5.5 9.5 L12 16 L18.5 9.5',
    'edit': 'M14 5 L19 10 M4 20 L9 19 L20 8 Q22 6 19 3 Q18 2 16 4 L5 15 Z M5 15 L9 19',
    'repeat': 'M4 9 V8 Q4 5 7 5 H20 M17 2 L20 5 L17 8 M20 15 V16 Q20 19 17 19 H4 M7 16 L4 19 L7 22',
    'palette': 'M21 11 C21 5 16 2 11 3 C5 3 2 8 3 14 C4 19 8 21 12 21 Q15 21 14 18 Q13 16 16 16 H18 Q21 16 21 11 Z M7 10 H7.2 M10 6.5 H10.2 M15 7 H15.2 M18 11 H18.2',
}
# Append state variants so all existing icon IDs remain stable.
paths['heart_filled'] = paths['heart']
paths['moon_filled'] = paths['moon']
FILLED = {'play', 'pause', 'prev', 'skip', 'more', 'heart_filled', 'moon_filled'}
def tessellate(d):
 tokens=re.findall(r'[A-Za-z]|[-+]?(?:\d*\.\d+|\d+)',d);i=0;cmd=None;p=(0.,0.);start=p;parts=[];part=[]
 def add(q):
  nonlocal p
  p=q;part.append(q)
 while i<len(tokens):
  if tokens[i].isalpha():cmd=tokens[i];i+=1
  c=cmd.upper();rel=cmd.islower()
  if c=='Z':
   add(start);cmd=None;continue
  count={'M':2,'L':2,'H':1,'V':1,'Q':4,'C':6,'A':7}[c]
  v=list(map(float,tokens[i:i+count]));i+=count;old=p
  def point(a,b):return (a+(old[0] if rel else 0),b+(old[1] if rel else 0))
  if c=='M':
   if part:parts.append(part)
   part=[];add(point(*v));start=p;cmd='l' if rel else 'L'
  elif c=='L':add(point(*v))
  elif c=='H':add((v[0]+(old[0] if rel else 0),old[1]))
  elif c=='V':add((old[0],v[0]+(old[1] if rel else 0)))
  elif c in ('Q','C'):
   controls=[old]+[point(*v[j:j+2]) for j in range(0,len(v),2)]
   for k in range(1,17):
    t=k/16;work=controls[:]
    while len(work)>1:work=[((1-t)*a[0]+t*b[0],(1-t)*a[1]+t*b[1]) for a,b in zip(work,work[1:])]
    add(work[0])
  elif c=='A':
   rx,ry,rotation,large,sweep,x,y=v;end=point(x,y);rx=abs(rx);ry=abs(ry);phi=math.radians(rotation);co=math.cos(phi);si=math.sin(phi)
   dx=(old[0]-end[0])/2;dy=(old[1]-end[1])/2;xp=co*dx+si*dy;yp=-si*dx+co*dy
   lam=xp*xp/(rx*rx)+yp*yp/(ry*ry)
   if lam>1:rx*=math.sqrt(lam);ry*=math.sqrt(lam)
   den=rx*rx*yp*yp+ry*ry*xp*xp
   factor=(-1 if large==sweep else 1)*math.sqrt(max(0,(rx*rx*ry*ry-den)/den)) if den else 0
   cxp=factor*rx*yp/ry;cyp=-factor*ry*xp/rx
   cx=co*cxp-si*cyp+(old[0]+end[0])/2;cy=si*cxp+co*cyp+(old[1]+end[1])/2
   a=math.atan2((yp-cyp)/ry,(xp-cxp)/rx);b=math.atan2((-yp-cyp)/ry,(-xp-cxp)/rx);delta=b-a
   if sweep and delta<0:delta+=2*math.pi
   if not sweep and delta>0:delta-=2*math.pi
   steps=max(4,math.ceil(abs(delta)*8))
   for k in range(1,steps+1):
    t=a+delta*k/steps;add((cx+co*rx*math.cos(t)-si*ry*math.sin(t),cy+si*rx*math.cos(t)+co*ry*math.sin(t)))
 if part:parts.append(part)
 return parts
names=list(paths)
h=['/* Generated Shema icons. LVGL 8; no external icon/font dependency. */','#pragma once','#include "lvgl.h"','typedef enum {']+['    SHEMA_ICON_'+n.upper()+',' for n in names]+['    SHEMA_ICON_COUNT','} shema_icon_id_t;','lv_obj_t *shema_icon_create(lv_obj_t *parent, shema_icon_id_t icon, unsigned size, lv_color_t color);','void shema_icon_set_color(lv_obj_t *icon, lv_color_t color);','void shema_icon_set_symbol(lv_obj_t *icon, shema_icon_id_t id, unsigned size);']
# Rasterise every icon to an anti-aliased 8-bit coverage mask (LV_IMG_CF_ALPHA_8BIT).
# Strokes are rendered from the float path with an exact distance field, so round
# caps/joins and curves stay smooth; the image is tinted by img_recolor.
STROKE=2.0
def segments(d,scale):
 segs=[]
 for part in tessellate(d):
  pts=[(x*scale,y*scale) for x,y in part]
  if len(pts)==1:pts.append(pts[0])
  segs+=list(zip(pts,pts[1:]))
 return segs
def seg_dist(px,py,a,b):
 ax,ay=a;bx,by=b;dx=bx-ax;dy=by-ay;l=dx*dx+dy*dy
 t=0 if l==0 else max(0,min(1,((px-ax)*dx+(py-ay)*dy)/l))
 qx=ax+t*dx-px;qy=ay+t*dy-py
 return math.sqrt(qx*qx+qy*qy)
def inside(px, py, polygons):
 winding = False
 for part in polygons:
  for a,b in zip(part, part[1:] + part[:1]):
   if (a[1] > py) != (b[1] > py) and px < (b[0]-a[0])*(py-a[1])/(b[1]-a[1])+a[0]:
    winding = not winding
 return winding

def raster(d,size,filled=False):
 scale=size/24;half=STROKE*scale/2;segs=segments(d,scale);out=bytearray(size*size)
 polygons = [[(x*scale,y*scale) for x,y in part] for part in tessellate(d)] if filled else []
 for y in range(size):
  for x in range(size):
   cx=x+.5;cy=y+.5
   dist=min(seg_dist(cx,cy,a,b) for a,b in segs)
   coverage = .5 + (dist if inside(cx,cy,polygons) else -dist) if filled else half+.5-dist
   out[y*size+x]=round(max(0.,min(1.,coverage))*255)
 return out
def png(path,w,h,rows):
 import zlib,struct
 raw=b''.join(b'\0'+bytes(r) for r in rows)
 ch=lambda t,d:struct.pack('>I',len(d))+t+d+struct.pack('>I',zlib.crc32(t+d)&0xffffffff)
 Path(path).write_bytes(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw,9))+ch(b'IEND',b''))
c=['/* Generated by generate_icons.py; do not edit. Anti-aliased A8 masks, tinted with img_recolor. */','#include "shema_icons.h"']
masks={}
for name,d in paths.items():
 fill = '#E8BE78' if name in FILLED else 'none'
 stroke = 'none' if name in FILLED else '#E8BE78'
 (P/'icons'/f'{name}.svg').write_text(f'<svg xmlns="http://www.w3.org/2000/svg" width="24" height="24" viewBox="0 0 24 24"><path d="{d}" fill="{fill}" stroke="{stroke}" stroke-width="{STROKE}" stroke-linecap="round" stroke-linejoin="round"/></svg>')
 for size in (24,28,32):
  m=raster(d,size,name in FILLED);masks[(name,size)]=m
  body=',\n'.join('    '+','.join('0x%02x'%v for v in m[r*size:(r+1)*size]) for r in range(size))
  c.append(f'static const uint8_t {name}_{size}_map[] = {{\n{body}\n}};')
  c.append(f'static const lv_img_dsc_t {name}_{size} = {{ .header.cf = LV_IMG_CF_ALPHA_8BIT, .header.always_zero = 0, .header.w = {size}, .header.h = {size}, .data_size = sizeof({name}_{size}_map), .data = {name}_{size}_map }};')
for size in (24,28,32):c.append(f'static const lv_img_dsc_t *const icons_{size}[SHEMA_ICON_COUNT] = {{'+','.join(f'&{n}_{size}' for n in names)+'};')
c+=["""lv_obj_t *shema_icon_create(lv_obj_t *parent, shema_icon_id_t id, unsigned size, lv_color_t color) {
    if ((unsigned)id >= SHEMA_ICON_COUNT) return NULL;
    if (size != 24 && size != 28 && size != 32) size = 24;
    const lv_img_dsc_t *src = (size == 32 ? icons_32 : size == 28 ? icons_28 : icons_24)[id];
    lv_obj_t *obj = lv_img_create(parent);
    lv_obj_remove_style_all(obj);
    lv_img_set_src(obj, src);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    shema_icon_set_color(obj, color);
    return obj;
}
void shema_icon_set_symbol(lv_obj_t *icon, shema_icon_id_t id, unsigned size) {
    if (!icon || (unsigned)id >= SHEMA_ICON_COUNT) return;
    const lv_img_dsc_t *src = (size == 32 ? icons_32 : size == 28 ? icons_28 : icons_24)[id];
    lv_img_set_src(icon, src);
}
void shema_icon_set_color(lv_obj_t *icon, lv_color_t color) {
    /* shema_icon_create() returns NULL for an out-of-range id, and callers commonly pass
       lv_obj_get_child(btn, 0), which is NULL on a childless button. */
    if (!icon) return;
    lv_obj_set_style_img_recolor(icon, color, 0);
    lv_obj_set_style_img_recolor_opa(icon, LV_OPA_COVER, 0);
}
"""]
# Compact preview: each cell shows 24px and 32px artwork, at 3x nearest-neighbour.
Z=3;cols=7;cw=80;ch=48;W=cols*cw*Z;H=math.ceil(len(names)/cols)*ch*Z
bg=(0x08,0x0d,0x13);fg=(0xE8,0xBE,0x78)
rows=[]
for Y in range(H):
 row=bytearray();cy=Y//Z
 for X in range(W):
  cx=X//Z;ni=(cy//ch)*cols+cx//cw;xx=cx%cw;yy=cy%ch
  size=24 if xx<40 else 32;ix=xx-(8 if size==24 else 44);iy=yy-(ch-size)//2
  a=masks[(names[ni],size)][iy*size+ix]/255 if ni<len(names) and 0<=ix<size and 0<=iy<size else 0
  row+=bytes(round(b*(1-a)+f*a) for b,f in zip(bg,fg))
 rows.append(row)
png(P/'icons_preview.png',W,H,rows)
(UI/'shema_icons.h').write_text('\n'.join(h)+'\n');(UI/'shema_icons.c').write_text('\n'.join(c)+'\n')
(P/'icons.json').write_text(json.dumps({'sizes':[24,28,32],'icons':names,'source':'Original project-authored SVG paths in generate_icons.py','stroke_width':STROKE,'filled_icons':sorted(FILLED),'rendering':'LVGL 8 anti-aliased A8 images, tinted via img_recolor'},indent=2)+'\n')
print(f'Exported {len(names)} SVG icons and {len(names)*3} LVGL icon variants')

# Refresh the asset manifest checksums.
manifest = json.loads((P/'manifest.json').read_text())
manifest['icons'] = len(names)
files = ['README.md', 'generate_icons.py', 'icons.json', 'icons_preview.png']
files += [f'icons/{name}.svg' for name in names]
manifest['files'] = {name: {'bytes': (P/name).stat().st_size,
                          'sha256': hashlib.sha256((P/name).read_bytes()).hexdigest()}
                     for name in sorted(files)}
(P/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
