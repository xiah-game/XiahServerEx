import os
import re

for root, _, files in os.walk('D:/xiahold/XiahServerEx'):
    for file in files:
        if file.endswith('.cpp') or file.endswith('.h'):
            path = os.path.join(root, file)
            with open(path, 'r', encoding='utf-8', errors='ignore') as f:
                content = f.read()

            # Find all ->payloadSize = <number>;
            # and replace it with ->payloadSize = <name>Buf.size() - sizeof(PACKET_HEADER);
            # by looking at the line or previous line for the vector name.
            
            lines = content.split('\n')
            changed = False
            for i in range(len(lines)):
                if '->payloadSize =' in lines[i] and not 'sizeof(' in lines[i]:
                    # Need to extract the buf name
                    m = re.search(r'\(\w+\*\)(\w+)Buf\.data\(\)', lines[i])
                    if not m and i > 0:
                        m = re.search(r'\(\w+\*\)(\w+)Buf\.data\(\)', lines[i-1])
                    if not m:
                        m = re.search(r'\(\w+\*\)(\w+)\.data\(\)', lines[i])

                    if m:
                        buf_name = m.group(1)
                        if 'Buf' not in buf_name and not m.group(0).endswith('Buf.data()'):
                            buf_name = buf_name
                        else:
                            buf_name = buf_name + 'Buf'
                            
                        # Replace the number with the new formula
                        lines[i] = re.sub(r'->payloadSize\s*=\s*\d+\s*;', f'->payloadSize = {buf_name}.size() - sizeof(PACKET_HEADER);', lines[i])
                        changed = True

            if changed:
                with open(path, 'w', encoding='utf-8') as f:
                    f.write('\n'.join(lines))
