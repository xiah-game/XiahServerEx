import os

file_path = r'd:\xiahold\XiahServerEx\UnitServer.cpp'
with open(file_path, 'rb') as f:
    content = f.read()

target = b'LOG("[UnitServer] Granted "'
idx = content.find(target)

if idx != -1:
    end_idx = content.find(b'}', idx)
    if end_idx != -1:
        new_content = content[:end_idx + 1] + b'\r\n\r\n'
        
        with open(file_path, 'wb') as f:
            f.write(new_content)
        print("Fixed UnitServer.cpp")
    else:
        print("Could not find }")
else:
    print("Could not find target string")
