import os, subprocess, struct, random, shutil
from PIL import Image, ImageDraw, ImageFont
import sys
R=sys.argv[1] if len(sys.argv) > 1 else 'testsd'
shutil.rmtree(R, ignore_errors=True)
os.makedirs(R)
FONT='/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf'
def cover(path, c1, c2, text, size=800):
    im=Image.new('RGB',(size,size),c1); d=ImageDraw.Draw(im)
    for y in range(size):
        t=y/size; d.line([(0,y),(size,y)], fill=tuple(int(c1[i]*(1-t)+c2[i]*t) for i in range(3)))
    for i in range(6):
        r=random.randint(60,300); x=random.randint(0,size); y=random.randint(0,size)
        d.ellipse([x-r,y-r,x+r,y+r], outline=(255,255,255), width=6)
    f=ImageFont.truetype(FONT, size//9)
    d.text((size*0.08,size*0.7), text, font=f, fill=(255,255,255))
    im.save(path, quality=88)
def tone(path, secs, freq, meta, cover_path=None, extra=()):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    cmd=['ffmpeg','-loglevel','error','-y','-f','lavfi','-i',f'sine=frequency={freq}:duration={secs}']
    if cover_path: cmd+=['-i',cover_path,'-map','0:a','-map','1:v','-c:v','mjpeg','-disposition:v','attached_pic','-metadata:s:v','title=Album cover','-metadata:s:v','comment=Cover (front)']
    for k,v in meta.items(): cmd+=['-metadata',f'{k}={v}']
    cmd+=list(extra)+['-ac','2',path]
    subprocess.run(cmd, check=True)
tmp='/tmp/covers'; os.makedirs(tmp, exist_ok=True)
random.seed(4)
# 1. Кино — folder cover, mp3 id3v2.3
d=f'{R}/Music/Кино/1988 - Группа крови'
os.makedirs(d); cover(f'{d}/cover.jpg',(160,20,20),(20,0,0),'КИНО')
for i,t in enumerate(['Группа крови','Закрой за мной дверь','Война','Спокойная ночь']):
    tone(f'{d}/{i+1:02d} - {t}.mp3', 12, 220+i*40, {'title':t,'artist':'Кино','album':'Группа крови','date':'1988','track':f'{i+1}/4'}, extra=['-id3v2_version','3','-b:a','128k'])
d=f'{R}/Music/Кино/1989 - Звезда по имени Солнце'
os.makedirs(d); cover(f'{d}/folder.jpg',(230,170,30),(120,40,0),'ЗВЕЗДА')
for i,t in enumerate(['Звезда по имени Солнце','Невесёлая песня','Пачка сигарет']):
    tone(f'{d}/{i+1:02d}. {t}.mp3', 9, 300+i*50, {'title':t,'artist':'Кино','album':'Звезда по имени Солнце','date':'1989','track':str(i+1)}, extra=['-id3v2_version','4','-b:a','192k'])
# 2. FLAC with embedded picture
cover(f'{tmp}/okc.jpg',(40,90,160),(200,220,240),'OK')
for i,t in enumerate(['Airbag','Paranoid Android','Subterranean Homesick Alien','Exit Music']):
    tone(f'{R}/Music/Radiohead/OK Computer/{i+1:02d} {t}.flac', 10, 330+i*30, {'title':t,'artist':'Radiohead','album_artist':'Radiohead','album':'OK Computer','date':'1997','track':str(i+1)}, cover_path=f'{tmp}/okc.jpg')
# 3. Multi-disc with embedded APIC in mp3
cover(f'{tmp}/wall.jpg',(235,235,235),(170,170,170),'THE WALL')
for disc in (1,2):
    for i in range(3):
        t=f'Another Brick part {disc}{i+1}'
        tone(f'{R}/Music/Pink Floyd/The Wall/CD{disc}/{i+1:02d} - {t}.mp3', 8, 250+disc*60+i*20, {'title':t,'artist':'Pink Floyd','album':'The Wall','date':'1979','track':str(i+1),'disc':str(disc)}, cover_path=f'{tmp}/wall.jpg', extra=['-id3v2_version','3'])
# 4. Various artists ogg compilation with folder.png
d=f'{R}/Music/Сборники/Летние хиты 2024'
os.makedirs(d); cover(f'{d}/front.png',(20,170,140),(10,40,90),'ЛЕТО',600)
for i,(a,t) in enumerate([('Artist One','Sunrise'),('Вторая группа','Море'),('Third','Waves'),('Четвёртый','Пляж')]):
    tone(f'{d}/{i+1:02d}.ogg', 7, 400+i*35, {'title':t,'artist':a,'album':'Летние хиты 2024','date':'2024','tracknumber':str(i+1)}, extra=['-c:a','libvorbis'])
# 5. untagged downloads
for n in ['Ария - Беспечный ангел','Daft Punk - One More Time','03 Untitled track']:
    tone(f'{R}/Download/{n}.mp3', 6, 500, {}, extra=['-map_metadata','-1','-id3v2_version','0','-write_id3v1','0'])
# 6. wav with INFO
tone(f'{R}/Music/Demo/Запись с диктофона.wav', 5, 610, {'title':'Демо 1','artist':'Моя группа','album':'Демки'})
tone(f'{R}/Music/Demo/Demo 2.wav', 4, 640, {'title':'Демо 2','artist':'Моя группа','album':'Демки'})
# 7. cp1251 ID3v1-only mp3
p=f'{R}/Music/Старое/ДДТ - Осень.mp3'
tone(p, 6, 700, {}, extra=['-map_metadata','-1','-id3v2_version','0','-write_id3v1','0'])
def pad(s,n): b=s.encode('cp1251'); return b[:n]+b'\0'*(n-len(b[:n]))
with open(p,'ab') as f: f.write(b'TAG'+pad('Осень',30)+pad('ДДТ',30)+pad('Актриса Весна',30)+b'1992'+b'\0'*28+b'\0'+bytes([3])+bytes([12]))
# 8. should be skipped
tone(f'{R}/Roms/GBA/game/bgm.ogg', 3, 800, {'title':'ROM BGM'}, extra=['-c:a','libvorbis'])
tone(f'{R}/.hidden/secret.mp3', 3, 800, {'title':'HIDDEN'})
# 9. more artists to fill the lists
names=['Аквариум','Земфира','Сплин','Мумий Тролль','Наутилус Помпилиус','Massive Attack','Portishead','Björk','The Cure','Depeche Mode']
cols=[(90,40,140),(200,60,90),(30,120,60),(220,120,30),(40,40,40),(10,10,90),(140,140,30),(200,40,160),(60,60,60),(150,20,20)]
for k,(n,c) in enumerate(zip(names,cols)):
    d=f'{R}/Music/{n}/Альбом {k+1}'
    os.makedirs(d)
    if k%3!=2: cover(f'{d}/cover.jpg',c,(0,0,0),n[:8].upper(),500)
    for i in range(2):
        tone(f'{d}/{i+1:02d} Песня {i+1}.mp3', 4, 200+k*20+i*10, {'title':f'Песня номер {i+1} очень длинное название трека чтобы проверить обрезку','artist':n,'album':f'Альбом {k+1}','date':str(1990+k),'track':str(i+1)})
print('ok')
