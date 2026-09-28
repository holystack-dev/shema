from pathlib import Path
from html import escape
import json
import base64
P=Path(__file__).parent
BG='#080D13'; CARD='#17212B'; GOLD='#E8BE78'; WHITE='#F7F1E6'; MUTED='#AFBDC7'
paths={
'book':'M3 5 Q8 3 12 6 Q16 3 21 5 V20 Q16 18 12 21 Q8 18 3 20 Z M12 6 V21',
'year':'M5 5 H19 V21 H5 Z M8 3 V7 M16 3 V7 M5 10 H19 M9 15 L11 17 L16 12',
'folder':'M3 6 H10 L12 9 H21 V20 H3 Z',
'heart':'M12 21 L4 13 C-2 5 7 1 12 7 C17 1 26 5 20 13 Z',
'back':'M15 5 L8 12 L15 19',
'next':'M9 5 L16 12 L9 19',
'play':'M8 4 L20 12 L8 20 Z',
'pause':'M8 5 V19 M16 5 V19',
'prev':'M5 5 V19 M19 5 L8 12 L19 19 Z',
'skip':'M19 5 V19 M5 5 L16 12 L5 19 Z',
'more':'M5 12 h.01 M12 12 h.01 M19 12 h.01',
'recent':'M3 11 A9 9 0 1 1 5 19 M3 4 V11 H10 M12 7 V13 L16 15',
'settings':'M12 3 V6 M12 18 V21 M3 12 H6 M18 12 H21 M5.6 5.6 L7.8 7.8 M16.2 16.2 L18.4 18.4 M5.6 18.4 L7.8 16.2 M16.2 7.8 L18.4 5.6 M17 12 A5 5 0 1 1 7 12 A5 5 0 1 1 17 12',
'moon':'M20 15 A9 9 0 0 1 9 3 A9 9 0 0 0 20 15',
'volume':'M3 9 H7 L12 5 V19 L7 15 H3 Z M16 8 Q21 12 16 16',
'minus':'M5 12 H19',
'plus':'M5 12 H19 M12 5 V19',
'shuffle':'M3 6 H6 L18 18 H21 M17 14 L21 18 L17 22 M3 18 H6 L10 14 M14 10 L18 6 H21 M17 2 L21 6 L17 10',
'check':'M4 12 L9 17 L20 6'}
def icon(n,x,y,size=24,col=GOLD):
 return f'<svg x="{x}" y="{y}" width="{size}" height="{size}" viewBox="0 0 24 24"><path d="{paths[n]}" fill="none" stroke="{col}" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round"/></svg>'
def txt(s,x,y,size=18,col=WHITE,anchor='middle',serif=False):
 return f'<text x="{x}" y="{y}" text-anchor="{anchor}" fill="{col}" font-size="{size}" font-family="{("Georgia,serif" if serif else "Arial,sans-serif")}">{escape(s)}</text>'
def rect(x,y,w,h,fill=CARD,r=18):return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{r}" fill="{fill}"/>'
def link(s,target):return f'<a href="#{target}" data-screen="{target}">{s}</a>'
def button(n,x,y,target,size=64,fill=CARD):return link(rect(x,y,size,size,fill,size/2)+icon(n,x+(size-26)/2,y+(size-26)/2,26,BG if fill==GOLD else GOLD),target)
def header(title):return button('back',60,52,'back')+txt(title,211,91,19 if len(title)>13 else 21)
def foot(label='More',target='home',n='more'):return link(rect(116,280,128,64)+icon(n,133,299,24)+txt(label,191,318,17),target)
def row(title,sub,y,n,target):return link(rect(60,y+8,240,64)+icon(n,76,y+28)+txt(title,112,y+34,18,WHITE,'start')+txt(sub,112,y+55,14,MUTED,'start'),target)
def screen(body):return '<svg xmlns="http://www.w3.org/2000/svg" width="360" height="360" viewBox="0 0 360 360"><defs><clipPath id="round"><circle cx="180" cy="180" r="180"/></clipPath></defs><g clip-path="url(#round)"><circle cx="180" cy="180" r="180" fill="'+BG+'"/><circle cx="180" cy="180" r="174" fill="none" stroke="#26313B"/>'+body+'</g></svg>'
S={}
body=txt('Shema',180,66,31,WHITE,serif=True)+txt('NRSV · Audio Bible',180,88,14,MUTED)
for name,n,x,y,dest in [('Bible','book',66,104,'books'),('In a Year','year',194,104,'year'),('Library','folder',66,188,'library'),('More','more',194,188,'more')]:
 body+=link(rect(x,y,100,72)+icon(n,x+38,y+10)+txt(name,x+50,y+58,16),dest)
body+=foot('Resume','player','play');S['home']=body
S['more']=header('Your space')+row('Recent','Pick up where you left off',116,'recent','recent')+row('Favourites','Your saved listening',192,'heart','favourites')+foot('Settings','settings','settings')
S['books']=header('Bible')+row('Old Testament','Genesis – Malachi',116,'book','old-books')+row('New Testament','Matthew – Revelation',192,'book','book-list')+foot('NRSV','versions','more')
S['book-list']=header('New Testament')+row('John','21 chapters',116,'book','chapters')+row('Acts','28 chapters',192,'book','chapters')+foot('More','book-list-2','next')
S['book-list-2']=header('New Testament')+row('Romans','16 chapters',116,'book','chapters')+row('1 Corinthians','16 chapters',192,'book','chapters')+foot('Previous','book-list','back')
body=header('John')
for i in range(6):
 x=72+(i%3)*76;y=124+(i//3)*76
 body+=link(rect(x,y,64,64,GOLD if i==2 else CARD)+txt(str(i+1),x+32,y+42,25,BG if i==2 else WHITE),'player')
S['chapters']=body+foot('7–12','chapters-2','next')
S['chapters-2']=header('John')+''.join(link(rect(72+(i%3)*76,124+(i//3)*76,64,64)+txt(str(i+7),104+(i%3)*76,166+(i//3)*76,25),'player') for i in range(6))+foot('1–6','chapters','back')
S['player']=txt('NRSV',180,47,14,MUTED)+txt('John',180,82,31,WHITE,serif=True)+txt('Chapter 3',180,108,20,GOLD)+rect(76,129,208,5,'#33404A',2)+rect(76,129,78,5,GOLD,2)+txt('1:48',76,156,14,MUTED,'start')+txt('4:49',284,156,14,MUTED,'end')+button('prev',64,179,'player')+button('pause',140,171,'player-paused',80,GOLD)+button('skip',232,179,'player')+button('back',104,267,'home')+button('more',192,267,'player-options')
S['player-paused']=S['player'].replace(paths['pause'],paths['play']).replace('#player-paused','#player').replace('data-screen="player-paused"','data-screen="player"')
S['player-options']=header('Playback')+row('Volume','70% · tap to adjust',116,'volume','volume')+row('Sleep timer','Off',192,'moon','sleep')+foot('Save','favourites','heart')
S['volume']=header('Volume')+txt('70%',180,171,44,GOLD)+rect(76,204,208,8,'#33404A',4)+rect(76,204,145,8,GOLD,4)+foot('Done','player','check')
S['sleep']=header('Sleep timer')+row('15 minutes','Stop after this time',116,'moon','player')+row('30 minutes','Stop after this time',192,'moon','player')+foot('Off','player','check')
S['year']=header('In a Year')+row('Early World','Days 1–5',116,'year','days')+row('Patriarchs','Days 6–26',192,'year','days')+foot('English','versions','more')
S['days']=header('Early World')+row('Day 1','Genesis 1–2',116,'check','player')+row('Day 2','Continue your journey',192,'play','player')+foot('Intros','intros','book')
S['intros']=header('Introductions')+row('Introduction','Early World',116,'play','player')+row('Checkpoints','Explore the story',192,'book','player')+foot('Back','year','back')
S['library']=header('Library')+row('Prayers','Browse folder',116,'folder','folder')+row('Music','Browse folder',192,'folder','folder')+foot('A–Z','letters','more')
S['folder']=header('Prayers')+row('Morning prayer','Ready to listen',116,'play','player')+row('Evening prayer','Ready to listen',192,'play','player')+foot('Shuffle','player','shuffle')
S['letters']=header('Jump to letter')+''.join(link(rect(72+(i%3)*76,124+(i//3)*76,64,64)+txt(c,104+(i%3)*76,166+(i//3)*76,25),'folder') for i,c in enumerate('ABCDEF'))+foot('More','letters','next')
S['recent']=header('Recent')+row('John · Chapter 3','Resume at 1:48',116,'recent','player')+row('Day 1','Bible in a Year',192,'year','player')+foot('Home','home','back')
S['favourites']=header('Favourites')+row('Bible','Saved chapters',116,'book','saved')+row('In a Year','Saved days',192,'year','saved')+foot('Library','saved','folder')
S['saved']=header('Saved chapters')+row('John · Chapter 3','NRSV',116,'heart','player')+row('Psalm 23','NRSV',192,'heart','player')+foot('Edit','saved-edit','more')
S['saved-edit']=header('Edit saved')+row('John · Chapter 3','Tap to remove · demo',116,'heart','saved')+row('Psalm 23','Tap to remove · demo',192,'heart','saved')+foot('Done','saved','check')
S['settings']=header('Settings')+row('Version','English · NRSV',116,'book','versions')+row('Brightness','70%',192,'settings','brightness')+foot('More','settings-2','next')
S['settings-2']=header('Settings')+row('Loop Bible','Off',116,'recent','settings-2')+row('Swipe to go back','On',192,'back','settings-2')+foot('More','settings-3','next')
S['settings-3']=header('Settings')+row('Screen timeout','30 seconds',116,'moon','settings-3')+row('Auto power-off','10 minutes',192,'settings','settings-3')+foot('More','settings-4','next')
S['settings-4']=header('Settings')+row('Theme colour','Champagne',116,'settings','settings-4')+row('Played history','Manage listening history',192,'recent','recent')+foot('Home','home','back')
S['versions']=header('Version')+row('English','NRSV',116,'check','home')+row('Malayalam','POC Dramatized',192,'book','home')+foot('Done','home','check')
S['no-card']=txt('Shema',180,77,31,WHITE,serif=True)+icon('folder',157,114,46)+txt('Insert your SD card',180,204,22)+txt('Your listening lives here.',180,233,16,MUTED)+foot('Settings','settings','settings')
# Supporting states retain the same touch geometry as the primary screens.
S['old-books']=header('Old Testament')+row('Genesis','50 chapters',116,'book','chapters')+row('Exodus','40 chapters',192,'book','chapters')+foot('More','old-books-2','next')
S['old-books-2']=header('Old Testament')+row('Leviticus','27 chapters',116,'book','chapters')+row('Numbers','36 chapters',192,'book','chapters')+foot('Previous','old-books','back')
for key,title,amount in [('volume','Volume',70),('brightness','Brightness',70)]:
 S[key]=header(title)+txt(str(amount)+'%',180,163,40,GOLD)+button('minus',100,188,key+'-down')+button('plus',196,188,key+'-up')+foot('Done','back','check')
S['empty-recent']=header('Recent')+icon('recent',157,130,46)+txt('A fresh beginning',180,215,22)+txt('Your listening will appear here.',180,244,14,MUTED)+foot('Browse','books','book')
S['empty-saved']=header('Favourites')+icon('heart',157,130,46)+txt('Keep a passage close',180,215,21)+txt('Save it from the playback menu.',180,244,14,MUTED)+foot('Browse','books','book')
S['screen-timeout']=header('Screen timeout')+row('30 seconds','Dim when idle',116,'moon','set-timeout-30')+row('1 minute','Dim when idle',192,'moon','set-timeout-60')+foot('Never','set-timeout-off','check')
S['power-off']=header('Auto power-off')+row('10 minutes','When not playing',116,'moon','set-power-10')+row('30 minutes','When not playing',192,'moon','set-power-30')+foot('Off','set-power-off','check')
S['theme']=header('Theme colour')+row('Champagne','Warm gold',116,'check','set-theme-gold')+row('Sage','Soft green',192,'check','set-theme-sage')+foot('Done','back','check')
S['history']=header('Played history')+row('Recent listening','View your progress',116,'recent','recent')+row('Clear history','Prototype data only',192,'more','history-clear')+foot('Back','back','back')
S['history-clear']=header('Clear history?')+txt('Clear demo progress?',180,166,20)+txt('Your audio files stay available.',180,195,14,MUTED)+button('back',100,216,'back')+button('check',196,216,'clear-history')
S['settings-3']=S['settings-3'].replace('data-screen="settings-3"','data-screen="screen-timeout"',1).replace('data-screen="settings-3"','data-screen="power-off"',1)
S['settings-4']=S['settings-4'].replace('data-screen="settings-4"','data-screen="theme"',1).replace('data-screen="recent"','data-screen="history"',1)
# Static battery sample: never a small touch target.
for key in list(S):
 S[key]=txt('82%',180,31,12,MUTED)+S[key]
splash=P.parents[1]/'audio_bible'/'main'/'assets'/'splash.png'
if splash.exists():
 data=base64.b64encode(splash.read_bytes()).decode()
 S['splash']=f'<image href="data:image/png;base64,{data}" x="0" y="0" width="360" height="360"/>'

for name,body in S.items():(P/f'{name}.svg').write_text(screen(body))
featured=(['splash'] if 'splash' in S else [])+['home','more','player','books','chapters','year','days','library','recent','favourites','settings','no-card','player-options','volume','empty-saved']
board='<svg xmlns="http://www.w3.org/2000/svg" width="1840" height="1840" viewBox="0 0 1840 1840"><rect width="1840" height="1840" fill="#EDE8DF"/>'+txt('Shema / A quieter way to listen',60,65,34,BG,'start',True)+txt('360 × 360 round display · generous touch targets · a complete navigation concept',60,101,18,'#52606A','start')
for i,name in enumerate(featured):
 x=128+(i%4)*408;y=145+(i//4)*409
 board+=f'<svg x="{x}" y="{y}" width="360" height="360" viewBox="0 0 360 360">'+screen(S[name])+'</svg>'+txt(name.replace('-',' ').title(),x+180,y+389,18,'#52606A')
board+='</svg>';(P/'overview.svg').write_text(board)
html='''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Shema — UI concept</title><style>*{box-sizing:border-box}body{margin:0;background:#ede8df;color:#15202a;font-family:Arial,sans-serif}main{max-width:1220px;margin:auto;padding:48px 24px}h1{font:44px Georgia;margin:12px 0}p{line-height:1.6;color:#52606a}.layout{display:flex;gap:48px;align-items:center;flex-wrap:wrap;margin:36px 0}#device{width:360px;height:360px;border-radius:50%;box-shadow:0 25px 60px #18232a40;flex-shrink:0}svg a{cursor:pointer}svg a:hover>rect{stroke:#e8be78;stroke-width:2}button,select{padding:14px 18px;border-radius:12px;border:1px solid #b9b5ac;background:#f8f5ef;font:16px Arial;color:#17212b}button{cursor:pointer}#gallery{display:grid;grid-template-columns:repeat(auto-fit,minmax(270px,1fr));gap:28px}figure{margin:0}figure img{width:100%;max-width:360px}figcaption{margin:12px 0 24px;color:#52606a}a{color:#765726}.note{max-width:580px}.current{display:flex;gap:20px;flex-wrap:wrap}svg a:focus-visible>rect{stroke:#fff;stroke-width:3}@media(max-width:420px){main{padding:28px 12px}#device{width:min(360px,94vw);height:auto;aspect-ratio:1}#device>svg{width:100%;height:100%}h1{font-size:34px}}</style><main><span>DESIGN CONCEPT / 02</span><h1>A quieter way to listen.</h1><p>Shema · Designed for the 360 × 360 circular display.</p><div class="layout"><div id="device"></div><div class="note"><h2>Explore the mockup</h2><p>Tap Bible, In a Year, Library, or More. Resume opens the player. The playback menu keeps volume, sleep, and favourites comfortably separated.</p><select id="picker" aria-label="Choose screen"></select> <button onclick="navigate('home')">Home</button><p>64 px minimum controls · 80 px main play button<br>Two spacious list rows per page · consistent line icons</p><p>This is a navigation and visual prototype. Tap the splash to begin. Try chapter selection, play/pause, volume, brightness, sleep and settings. Audio and SD-card content are sample data. Browser zoom is not a physical size calibration.</p></div></div><h2>Screen collection</h2><p>All eleven firmware page types are represented, with supporting menus and states. Content shown is sample data.</p><div id="gallery"></div><h2>Device captures</h2><p>Screens captured from the firmware on the device.</p><div class="current"><figure><img width="180" src="../../assets/screens/home.png" alt="Device home"><figcaption>Home</figcaption></figure><figure><img width="180" src="../../assets/screens/player.png" alt="Device player"><figcaption>Player</figcaption></figure><figure><img width="180" src="../../assets/screens/books.png" alt="Device books"><figcaption>Books</figcaption></figure><figure><img width="180" src="../../assets/screens/chapters.png" alt="Device chapters"><figcaption>Chapters</figcaption></figure><figure><img width="180" src="../../assets/screens/settings.png" alt="Device settings"><figcaption>Settings</figcaption></figure><figure><img width="180" src="../../assets/screens/list.png" alt="Device list"><figcaption>List</figcaption></figure></div><p><a href="overview.svg">Download overview</a> · <a href="README.md">Design measurements and source inventory</a></p></main><script>'''
html+='const screens='+json.dumps({k:screen(v) for k,v in S.items()})+';const featured='+json.dumps(featured)+';</script><script src="prototype.js"></script></html>'
(P/'index.html').write_text(html)
print(f'Created {len(S)} SVG screens, overview.svg and interactive index.html')
