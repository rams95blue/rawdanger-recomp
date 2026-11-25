# Characters, their things and how wet they are (US, SLUS-21501)

Found while making the wet look and the Debug tab (v0.9.5). Addresses are the US program's; the Japanese one differs.

## Characters

- Table at 0x5EAC80 (235 pointers). The player's number is at 0x48F3E4 (0 Joshua, 1 the other hero); both heroes' numbers
  are also their entries in the table.
- A character: +0x368 / +0x36C health and its most (100), +0x370 / +0x374 body temperature and its most (30000; the HUD's arrow
  turns blue when cold), +0xF0 the model (a list of bones, docs/CAMERA.md "The hero's head").
- Body temperature falls by `(clothes' cold) / k + (+0x18C4 - +0x18C0) / k` a step (0x14A7xx-0x14A924); the clothes' part comes
  from 0x1B3FB0, which walks the player's worn things.

## The things pool

- `gp - 0x4740` (gp = 0x493E70, so 0x48F730) holds the pool's address (0x621E80 here).
- Pool + 0x5C38: the sets, one list {+0 entries, +4 an entry's size (0xC0), +0xC how many (32)}; 0x1C4A50 (a0 the list, a1 the
  index) returns an entry.
- A set: +0xD 0 when in use, +0xE the character's number, +0x14 the things' list {+0 entries, +4 size (0x44), +0xC how many
  (256)}, +0x24 the first thing (an index; -1 none).
- A thing (0x44 bytes): +0 its index, +4 the one before, +8 the next (-1 the last), +0xC its definition, +0x14 its kind,
  +0x1C 1 when worn, +0x30 how wet (a float, 0 to 100: about 1 every 15 frames in the rain; the HUD shows "Dry" at 0).

## Seen in the tested places

| State | Player | Worn things (index: wetness) |
|---|---|---|
| Hotel ballroom (Joshua) | 0 | dry |
| Street in the rain (the other hero) | 1 | thing 3: 60 → 80 over 300 frames |
