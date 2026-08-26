# Splices a freshly generated kDefaultSeats[] body into RidingPlugin.cpp and reports what
# changed.  Byte-level so the file's UTF-8 BOM, LF endings and Chinese comments survive.
#
# Usage: python tools/apply_default_seats.py <new-body-file>
import sys, re

SRC = 'RidingPlugin.cpp'
OPEN = b'static const DefaultSeat kDefaultSeats[] = {\n'
CLOSE = b'};\n'

data = open(SRC, 'rb').read()
start = data.index(OPEN) + len(OPEN)
end = data.index(CLOSE, start)
old_body = data[start:end]
new_body = open(sys.argv[1], 'rb').read()

row = re.compile(rb'\{\s*"((?:[^"\\]|\\.|"\s*")*)"\s*,(.*?)\}')

def parse(body):
    seats = {}
    for m in row.finditer(body):
        lit = m.group(1)
        # collapse adjacent-literal splits, then unescape \xNN back to raw bytes
        lit = re.sub(rb'"\s*"', b'', lit)
        name = re.sub(rb'\\x([0-9A-Fa-f]{2})',
                      lambda h: bytes([int(h.group(1), 16)]), lit)
        nums = [f.strip().rstrip(b'f') for f in m.group(2).split(b',')]
        seats[name] = tuple(float(n) for n in nums)
    return seats

old, new = parse(old_body), parse(new_body)
FIELDS = ['mode', 'up', 'forward', 'lateral', 'posture', 'sit', 'ax', 'ay', 'az', 'base']

for name in sorted(set(old) | set(new)):
    label = name.decode('utf-8')
    if name not in new:
        print('REMOVED %s' % label); continue
    if name not in old:
        print('ADDED   %s  %s' % (label, new[name])); continue
    diffs = ['%s %g->%g' % (FIELDS[i], old[name][i], new[name][i])
             for i in range(len(FIELDS)) if abs(old[name][i] - new[name][i]) > 1e-9]
    if diffs:
        print('CHANGED %-8s %s' % (label, ', '.join(diffs)))

print('rows: %d -> %d' % (len(old), len(new)))
open(SRC, 'wb').write(data[:start] + new_body + data[end:])
print('spliced into %s' % SRC)
