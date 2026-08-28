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
        # col[17] = the body size columns 2/3/10/14 were confirmed correct at (added
        # 2026-08-28).  Optional: a cfg written before it existed says nothing about size,
        # and 0 means "unknown" = the seat is used unadapted, exactly as it was before the
        # feature existed.  Never invent a value here - guessing would silently move a seat
        # the author had already dialled in.
        ref = float(col[17]) if len(col) > 17 else 0.0
        # Valid seat modes are 0=exact 1=midpoint 2=neck 3=rear.  Mode 4 was the
        # rigid-body seat (removed 2026-08-27); its offsets are in mount-body space, so
        # such a row must never be baked in as a bone-anchor default.  Anything above 3
        # is likewise not a mode this DLL knows how to draw.
        if mode < 0 or mode > 3:
            sys.exit('%r has seat mode %d - only 0-3 (exact/midpoint/neck/rear) can be '
                     'baked as bone-anchor defaults' % (name, mode))
        rows.append((name, mode, up, forward, lateral, posture, sit, ax, ay, az, base, ref))

rows.sort(key=lambda r: r[0])          # byte order = stable across regenerations
out = io.StringIO()
for i, r in enumerate(rows):
    name = r[0]
    tail = '' if i == len(rows) - 1 else ','
    out.write('    { "%s", %d, %7.2ff, %7.2ff, %6.2ff, %d, %d, %6.3ff, %6.3ff, %6.3ff, %7.3ff, %5.3ff }%s  // %s\n'
              % (esc(name), r[1], r[2], r[3], r[4], r[5], r[6],
                 r[7], r[8], r[9], r[10], r[11], tail, name.decode('utf-8')))
# write BYTES: the source file is UTF-8, and a redirect on Windows would otherwise encode
# the trailing "// 名字" comments as CP936 and leave mixed encodings in a BOM'd UTF-8 file.
sys.stdout.buffer.write(out.getvalue().encode('utf-8'))
sys.stderr.write('%d rows\n' % len(rows))
