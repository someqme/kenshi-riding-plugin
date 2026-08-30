# race_by_name.py - the reverse of race_id.py: find records whose NAME contains a substring
# and print the stringID that follows, so a race can be identified by its English data name.
#
#   python tools\race_by_name.py crab garru
#
# Same hard rule as race_id.py: ⚠️ never guess a stringID's file suffix from its number.  Use
# this (or the mount log's `race=`) to obtain one.
#
# Record layout (RE_NOTES): [u32 len][name bytes][u32 len][stringID bytes].  The printable-run
# scan can land on a TAIL of the real name, so a length prefix is required right before the
# match (that is what `lead` searches for) and the following field must look like a stringID.

import glob
import os
import re
import struct
import sys

DATA = r'D:\steam\steamapps\common\Kenshi\data'
SID = re.compile(rb'^\d+-[A-Za-z_]+\.(base|mod)$')


def main(argv):
    if not argv:
        return 'usage: race_by_name.py <name substring> [substring ...]'
    pats = [p.lower() for p in argv]
    seen = set()
    for path in sorted(glob.glob(os.path.join(DATA, '*.base')) +
                       glob.glob(os.path.join(DATA, '*.mod'))):
        blob = open(path, 'rb').read()
        for m in re.finditer(rb'[ -~]{3,60}', blob):
            name = m.group(0)
            low = name.decode('ascii').lower()
            if not any(p in low for p in pats):
                continue
            for lead in range(0, 8):            # the hit may be a tail of the real string
                k = m.start() + lead - 4
                nlen = len(name) - lead
                if k < 0 or nlen < 3:
                    continue
                if struct.unpack_from('<I', blob, k)[0] != nlen:
                    continue
                j = k + 4 + nlen
                if j + 4 > len(blob):
                    break
                slen = struct.unpack_from('<I', blob, j)[0]
                if not (4 <= slen <= 40):
                    break
                sid = blob[j + 4:j + 4 + slen]
                if not SID.match(sid):
                    break
                row = (os.path.basename(path), name[lead:].decode('ascii'),
                       sid.decode('ascii'))
                if row not in seen:
                    seen.add(row)
                    print('%-16s %-34s %s' % row)
                break


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
