import sys

with open('d:/xiahold/XiahServerEx/MonsterAI.cpp', 'rb') as f:
    data = f.read()

# Replace hardcoded packet IDs

# 1. Map Leave: 0x3506 -> 0x3505
data = data.replace(b'id = 0x3506;', b'id = 0x3505;')

# 2. Stop Move: 0x350C -> 0x350B
data = data.replace(b'id = 0x350C;', b'id = 0x350B;')

# 3. PreAttack: 0x4004 -> 0x4003
data = data.replace(b'id = 0x4004;', b'id = 0x4003;')

# 4. Attack: 0x4006 -> 0x4005
data = data.replace(b'id = 0x4006;', b'id = 0x4005;')

# 5. Move: 0x3508 -> 0x3509
data = data.replace(b'id = 0x3508;', b'id = 0x3509;')

with open('d:/xiahold/XiahServerEx/MonsterAI.cpp', 'wb') as f:
    f.write(data)

print("Packet IDs fixed successfully.")
