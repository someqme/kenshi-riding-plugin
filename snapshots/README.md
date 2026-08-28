# Seat-table snapshots

`kDefaultSeats[]` in `RidingPlugin.cpp` is never hand-written: it is generated from a
`riding.cfg` by `tools/gen_default_seats.py` and spliced in by `tools/apply_default_seats.py`.
These files are the exact inputs the shipped tables were generated from — one per generation
that matters. They are development records; the released mod does **not** ship a `riding.cfg`
(the seats are compiled into the DLL, and a shipped cfg would override newer defaults and
clobber the player's own tuning).

| file | rows | table version | what it is |
|---|---|---|---|
| `riding_tuned_20260827.cfg` | 30 name rows | v3 | the last purely hand-tuned table |
| `riding_tuned_20260828_v7.cfg` | 42 name rows | v7 | adds column 18 (the body size each seat was confirmed at) — the table the size-adaptation law first shipped with |
| `riding_tuned_20260828_v9.cfg` | 21 race rows | v9 | name rows merged away: one standardized row per animal race |
| `riding_tuned_20260828_v11.cfg` | 21 race rows | v11 | current shipped table |

Intermediate generations (v6, v8, v10 and the early August files) are superseded and kept
out of the repo; each differed from a neighbour by a single documented change.

Row format is documented in the header of every snapshot and in `README.txt`.
