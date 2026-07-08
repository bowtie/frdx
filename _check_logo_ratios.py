import json, os
from PIL import Image
f = open('dist/mods/frdx/stations.json', 'r', encoding='utf-8')
d = json.load(f)
f.close()
stations = d if isinstance(d, list) else d.get('stations', d)
base = 'dist/mods/frdx'
for s in stations:
    sid = s['id']
    for st in s.get('streams', []):
        stid = st['id']
        logo = st.get('artwork_url', '') or s.get('metadata', {}).get('static_artwork_url', '')
        if logo and not logo.startswith('http'):
            path = os.path.join(base, logo)
            try:
                img = Image.open(path)
                w, h = img.size
                ratio = w / h
                print(f'{sid:15} {stid:10} {w}x{h} ratio={ratio:.3f} {logo}')
            except Exception as e:
                print(f'{sid:15} {stid:10} ERR {e} {logo}')
