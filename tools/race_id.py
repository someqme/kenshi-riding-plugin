# race_id.py - stringID -> the record NAME in front of it, read straight out of Kenshi's
# data files.  Offline; no game, no FCS.
#
#   python tools\race_id.py 3998-gamedata.base 56089-Newwworld.mod
#
# Why this exists: a race key is a stringID, and ⚠️ the NUMERIC half is only unique WITHIN the
# file that created the record while the suffix is that file's name - so the suffix CANNOT be
# derived from the number (guessing it has been proved wrong twice, see CLAUDE.md 内置默认座位
# v9).  Hard rule: only ever use a race key that the mount log's `race=` field printed, or that
# this script confirmed.
#
# Record layout (RE_NOTES): [u32 len][name bytes][u32 len][stringID bytes].
# ⚠️ The walk is bounded by the length prefixes and filtered to printable names on purpose:
# an unbounded scan reads whatever field happens to sit next to the id and prints it as a name.

import glob
import os
import struct
import sys

DATA = r'D:\steam\steamapps\common\Kenshi\data'


def main(targets):
    if not targets:
        return 'usage: race_id.py <stringID> [stringID ...]'
    for path in sorted(glob.glob(os.path.join(DATA, '*.base')) +
                       glob.glob(os.path.join(DATA, '*.mod'))):
        blob = open(path, 'rb').read()
        for t in targets:
            tb = t.encode('ascii')
            pos = 0
            while True:
                i = blob.find(tb, pos)
                if i < 0:
                    break
                pos = i + 1
                if i < 8:
                    continue
                # the stringID carries its own u32 length; require it to match exactly
                if struct.unpack_from('<I', blob, i - 4)[0] != len(tb):
                    continue
                j = i - 4                       # start of the stringID length field
                for nlen in range(1, 96):       # walk back over [nlen][name] ending at j
                    k = j - nlen - 4
                    if k < 0:
                        break
                    if struct.unpack_from('<I', blob, k)[0] != nlen:
                        continue
                    name = blob[k + 4:k + 4 + nlen]
                    if all(0x20 <= c < 0x7F for c in bytearray(name)):
                        print('%-16s %-24s %s' % (os.path.basename(path), t,
                                                  name.decode('ascii')))
                    break


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
