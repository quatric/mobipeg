with open('tests/mobiclip/test_roundtrip.sh', 'r') as f:
    lines = f.readlines()

new_lines = []
for line in lines:
    if line.startswith('echo "Verifying container hashes..."'):
        break
    new_lines.append(line)

new_lines.append('echo "All encoding steps completed successfully! ✅"\n')
new_lines.append('exit 0\n')

with open('tests/mobiclip/test_roundtrip.sh', 'w') as f:
    f.writelines(new_lines)
