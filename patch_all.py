#!/usr/bin/env python3
import os
import json

workshop_dir = os.path.expanduser('~/.local/share/Steam/steamapps/workshop/content/431960')

if not os.path.exists(workshop_dir):
    print(f"Workshop directory {workshop_dir} does not exist.")
    exit(1)

patched_count = 0
total_count = 0

for folder in os.listdir(workshop_dir):
    folder_path = os.path.join(workshop_dir, folder)
    if not os.path.isdir(folder_path):
        continue
    
    pj_path = os.path.join(folder_path, 'project.json')
    if not os.path.exists(pj_path):
        continue
        
    total_count += 1
    try:
        with open(pj_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
    except Exception as e:
        print(f"Error reading {pj_path}: {e}")
        continue
        
    changed = False
    
    # Check if this is a scene wallpaper
    wtype = data.get('type', '').lower()
    file_val = data.get('file', '')
    
    has_pkg = os.path.exists(os.path.join(folder_path, 'scene.pkg')) or os.path.exists(os.path.join(folder_path, 'gifscene.pkg'))
    
    # 1. Correct wrong scene.pkg/gifscene.pkg to scene.json
    if file_val in ('scene.pkg', 'gifscene.pkg'):
        data['file'] = 'scene.json'
        changed = True
        print(f"[{folder}] Correcting 'file' from '{file_val}' to 'scene.json'")
        
    # 2. Fix empty file value for scene packages
    elif not file_val and has_pkg:
        data['file'] = 'scene.json'
        changed = True
        print(f"[{folder}] Setting missing 'file' to 'scene.json' because package exists")
        
    # 3. Ensure type is set to scene if we have a package
    if not wtype and has_pkg:
        data['type'] = 'scene'
        changed = True
        print(f"[{folder}] Setting missing 'type' to 'scene' because package exists")

    if changed:
        try:
            with open(pj_path, 'w', encoding='utf-8') as f:
                json.dump(data, f, indent=4)
            patched_count += 1
        except Exception as e:
            print(f"Error writing {pj_path}: {e}")

print(f"\nDone! Scanned {total_count} wallpapers, auto-patched {patched_count} of them.")
