# Regenerates the kDefaultSeats[] table in RidingPlugin.cpp from a riding.cfg.
#
# The built-in table is what a fresh player gets before any riding.cfg exists, so it must
# be a byte-exact copy of the author's tuned cfg.  Species names MUST be emitted as
# explicit \xNN escapes: this source file is UTF-8 with a BOM and MSVC v100 has no
# /utf-8 switch, so a plain Chinese literal is re-encoded to code page 936 and can never
# compare equal to Character::getName() (which is UTF-8).
#
# Usage: python tools/gen_default_seats.py <riding.cfg>   > prints the table body
import sys, io

def esc(name_bytes):
    """Escape to a C string literal, splitting when a \\xNN would swallow the next char."""
    out = []
    prev_escape = False
    for b in name_bytes:
        c = chr(b)
        if 0x20 <= b < 0x7F and c not in '"\\':
            # A hex-ish ASCII char right after \xNN gets absorbed into the escape:
            # close the literal and open a new one (adjacent literals concatenate).
            if prev_escape and c in '0123456789abcdefABCDEF':
                out.append('" "')
            out.append(c)
            prev_escape = False
        else:
            out.append('\\x%02X' % b)
            prev_escape = True
    return ''.join(out)

rows = []
with open(sys.argv[1], 'rb') as f:
    for raw in f.read().split(b'\n'):
        line = raw.strip()
        if not line or line.startswith(b'#') or b'=' not in line:
            continue
        name, _, rest = line.partition(b'=')
        if name == b'defaults':
            continue
        col = rest.decode('ascii').split(',')
        if len(col) < 14:
            sys.exit('too few columns for %r' % name)
        mode    = int(col[0])
        up      = float(col[1])
        forward = float(col[2])
        # col[3] mount, col[5:8] roll/pitch/yaw = dead fields
        sit     = int(col[4])
        posture = int(col[8])
        lateral = float(col[9])
        ax, ay, az, base = (float(col[10]), float(col[11]),
                            float(col[12]), float(col[13]))
        if mode == 4:
            sys.exit('%r is mode 4 (rigid): its offsets are in mount-body space and must '
                     'not be baked as bone-anchor defaults' % name)
        rows.append((name, mode, up, forward, lateral, posture, sit, ax, ay, az, base))

rows.sort(key=lambda r: r[0])          # byte order = stable across regenerations
out = io.StringIO()
for i, r in enumerate(rows):
    name = r[0]
    tail = '' if i == len(rows) - 1 else ','
    out.write('    { "%s", %d, %7.2ff, %7.2ff, %6.2ff, %d, %d, %6.3ff, %6.3ff, %6.3ff, %7.3ff }%s  // %s\n'
              % (esc(name), r[1], r[2], r[3], r[4], r[5], r[6],
                 r[7], r[8], r[9], r[10], tail, name.decode('utf-8')))
# write BYTES: the source file is UTF-8, and a redirect on Windows would otherwise encode
# the trailing "// 名字" comments as CP936 and leave mixed encodings in a BOM'd UTF-8 file.
sys.stdout.buffer.write(out.getvalue().encode('utf-8'))
sys.stderr.write('%d rows\n' % len(rows))
