# NFS World road network: record fields, constraints, and whether a road can be appended

Research notes, 21 Nov 2026. nfsw.exe 32-bit, MD5 4c32936debffcbdc208d39502b79a56f, static addresses (base 0x400000).
Read with REA (Ghidra 12.1.4 provider, `procedure_pseudo_code` / `procedure_assembly` / `xrefs` / `read_bytes`) and
measured on the shipped `Data2015\TRACKS\frs-original\L5RA.BUN` (chunk 0x3B800). Builds on `nfsw-road-network.md`.
Measurement scripts (read-only, not kept in the repo): `rn_load.py`, `m1.py`..`m5.py` in the session scratchpad.

Tags: **[V]** verified (code line quoted, or a measurement over all shipped records), **[I]** inferred, **[?]** unknown.
"seg" = RNsg record, "node" = RNnd record. Offsets are hex. Game code always reads records as
`base + index * stride` from the globals 0xD11FB8 (RNpf), 0xD11FBC (RNnd), 0xD11FC0 (RNsg), 0xD11FC8 (RNrd), 0xD11FCC (RNlm).

## Topology in one paragraph

Nodes are points. A **road** (RNrd) is a simple chain of segments between two nodes whose degree is not 2
(junctions, degree 3..7, or dead ends, degree 1); every interior node of a road has degree 2 and both its
segments carry the same road index. Inside a junction, **connector segments** (flag bit 0x0001, road index 0xFFFF)
join two junction nodes. Measured: 2155 roads, all simple chains; 0 degree-2 nodes whose two segments differ in
road; all 5532 connector segments have junction nodes (degree > 2) at both ends; road end nodes have degree 1 (31),
3 (2346), 4 (1507), 5 (304), 6 (97), 7 (25). **[V]** (m3.py, m4.py, m5.py)

## 1. RNsg segment, 24 bytes

| off | type | meaning | tag / evidence |
|---|---|---|---|
| +00 | u16 | node A index | [V] 0x6D9920 `param_1[1]*0x20` / `*param_1*0x20` into RNnd; every segment is in both endpoint node lists (0 misses over 10898) |
| +02 | u16 | node B index | [V] same |
| +04 | u16 | arc length, metres = value * 0.015259 (1000/65535, float at 0xC86BA0 = 0x3C7A00FA) | [V] 0x6DF2C0 `*param_3 = (float)puVar5[2] * DAT_00c86ba0 + ...`; measured: Bezier arc length / (value*1000/65535) median 1.0002 (curved), 1.0006 (straight). Max shipped 259.6 m, field max 1000 m |
| +06 | s16 | road index (RNrd), -1 (0xFFFF) = connector segment | [V] 0x6D1290 `DAT_00d11fc8 + 4 + *(short *)(seg + 6) * 8`; 0x6D0F20 `if (-1 < sVar1 && sVar1 < (N-4)/2)`. 0xFFFF on exactly the 5532 segments with flag 0x0001 |
| +08 | u16 | the segment's own index | [V] all 10898 equal their index; used as identity: 0x6D2940 `param_2 != *(short *)(seg + 8)`, 0x6E6FD0 `local_c = puVar8[4]` |
| +0A | u16 | RNlm lane-map index | [V] 0x87D500 `*(short *)(seg + 10) * 0x20 + DAT_00d11fcc`; RNlm[+00] = lane count of node A's profile and RNlm[+10] = lane count of node B's profile for all 10898 segments |
| +0C | u16 | flags, see below | |
| +0E | u16 | Bezier handle length at node **B**, metres = value * 500/65535 (float at 0xCC5F7C = 0x3BFA00FA) | [V] 0x7E71C0 `(float)*(ushort *)(iVar9 + 0xe) * DAT_00cc5f7c * 0.007874016` paired with the s8 vector at +12 |
| +10 | u16 | Bezier handle length at node **A**, same unit | [V] 0x7E71C0, paired with +15 |
| +12 | s8[3] | unit direction at node B, * 127, pointing into the segment (towards A) | [V] 0x7E71C0 `(float)(int)*(char *)(iVar9 + 0x12) * fVar14`; measured norm 125.7..126.9, dot with (B-A) median -0.98 |
| +15 | s8[3] | unit direction at node A, * 127, pointing into the segment (towards B) | [V] same, dot with (B-A) median +0.98 |

**Curve** [V] 0x7E71C0 then 0x7DFD60 (cubic Bezier from 4 points). For travel from node B to node A
(direction byte 0; direction 1 swaps the ends and the two handle sets):
- flag 0x0100 set (curved, 10087 segs): P0 = B, P1 = B + v12/127 * h0E, P2 = A + v15/127 * h10, P3 = A
  (handles scaled by `lateral-offset chord / node chord` when a lane offset is applied, `fVar13` in 0x7E71C0).
- flag 0x0100 clear (straight, 811 segs): P1 = (2B + A)/3, P2 = (B + 2A)/3.
- Our decode reproduces the stored length to 0.1 % median over all segments (m4.py). **[V]**
- Handle / chord ratio sits near 1/3, the usual Bezier choice. **[V]** measured (raw +0E/+04 median 0.68 = 0.34 in metres)

**Flags +0C** (bit: count set in shipped data, out of 10898)

| bit | set | meaning | tag / evidence |
|---|---|---|---|
| 0x0001 | 5532 | connector segment inside a junction (no road). Skipped by "continue along road" 0x6D2940 `(*(byte *)(iVar1 + 0xc) & 1) == 0`; A* looks through it to the next road (0x6EC030) | [V] |
| 0x0002 | 1588 | data bit. Nav filter `+0x5e` rejects segments with it (0x6E6FD0 `(~(f >> 1) & 1) == 0`); flipped at load by the 0x7EBB00 fix-point (0x7EBA50 `*puVar1 ^= 2`) | [V] read/write, meaning [?] (one candidate: lane-direction / reverse marker) |
| 0x0004 | 0 | runtime: race-route direction (set by 0x6D1130 from consecutive route segments), cleared at load by 0x6D10C0 | [V] |
| 0x0008 | 0 | runtime: computed at load by 0x7EBB00 (clears it, then iterates 0x7EBA50 over both directions until stable) | [V] |
| 0x0010 | 3013 | data bit, read by race-route propagation 0x6D9770 together with 0x8000 | [V] read, meaning [?] |
| 0x0020 | 21 | data bit; nav filter `+0x5f` rejects when bit 0x20 != bit 0x02 (0x6E6FD0) | [V] read, meaning [?] |
| 0x0040 | 93 | **one-way A to B**: entering from node B is refused when the caller is not coming from A (0x6D92A0 `(f >> 6 & 1) != 0 && param_2 == 0 -> return 0`; 0x6E6FD0 `iVar11 != nodeA && (puVar9[6] & 0x40) != 0`) | [V] code, [I] that "A to B" is the allowed sense |
| 0x0080 | 0 | runtime: segment gated by its road's runtime byte RNrd+4 (0x6D1290, 0x6D92A0 case 2/4/6); cleared at load by 0x6D1230 | [V] |
| 0x0100 | 10087 | curved (use the stored handles), see Curve | [V] 0x7E71C0 `(... & 0x100) == 0` -> straight |
| 0x0200 | 1959 | profile at node B is reversed relative to the segment | [V] 0x7DFC10 `bVar2 = hi >> 1` for the node-B end; 0x899BB0 uses the same bits to pick `pf[1]` or `pf[0]-pf[1]` |
| 0x0400 | 1963 | profile at node A is reversed | [V] same (`hi >> 2`) |
| 0x0800 | 7 | exempt from area closures: 0x6EEC00 sets 0x1000/0x2000 only `if ((f >> 0xb & 1) == 0)` | [V] |
| 0x1000 | 0 | runtime: closed by an area record of type 0 (list 0xD5A1F8, count 0xD5A1F4, 24-byte records, byte +0x12 = type) | [V] 0x6EEC00 |
| 0x2000 | 0 | runtime: closed by an area record of type != 0 | [V] 0x6EEC00 |
| 0x4000 | 1070 | **cleared at load** by 0x6D10C0 `& 0xbfff`, then set at runtime by race-route code (0x6D9770 `uVar3 & 0x3fff \| 0x8000`) | [V] the shipped value is discarded |
| 0x8000 | 0 | runtime: on the active race route, cleared at load by 0x6D10C0 | [V] |

Nothing marks "traffic allowed" or "GPS allowed" as a single data bit. The nav filter 0x6D92A0(seg, enteringFromA,
navType, &penalty) applies, per nav type (nav object +0x7C): type 0 = one-way only; 2 = road gate (0x80) + one-way
+ closures 0x1000/0x2000; 3 = one-way + closures; 4 = gate + one-way + closures + a speed penalty; 6 = gate + closures
+ one-way; types 1 and 5 = no check. **[V]** code; the mapping of type numbers to traffic / racer / cop / GPS is **[?]**.

## 2. RNnd node, 32 bytes

| off | type | meaning | tag / evidence |
|---|---|---|---|
| +00 | f32[3] | position in collision axes (x = -game y, up, z = game x) | [V] 0x6F1790 bounds loop, 0x6D9920 |
| +0C | u16 | the node's own index | [V] all 7521; used as identity in 0x6DF2C0 `(int)*(short *)(*param_4 + 0xc) == (uint)*puVar5` |
| +0E | s16 | RNpf profile index, < 0 = default profile 0xD11F28 | [V] 0x6D13B0 `sVar1 = *(short *)(iVar2 + 0xe); if (-1 < sVar1) return sVar1 * 0x40 + DAT_00d11fb8`; all 7521 < 1213 |
| +10 | u8 | segment count | [V] 0x6D2940, 0x6EC030 `*(byte *)(node + 0x10)` |
| +11 | u8 | 0xAA padding | [V] all 7521 |
| +12 | u16[7] | segment indices, unused slots 0xAA 0xAA | [V] 0x6D2940 loop over `node + 0x12`; list consistent with seg +00/+02 both ways, sum of counts = 2 x segments (21796) |

Max segments per node = **7**: the record ends at +0x20 = 0x12 + 7*2, and the shipped maximum is 7 (25 nodes). **[V]**
The old note "u32 at +0C = node index" was half right: it is u16 index + s16 profile.

## 3. RNrd, RNpf, RNlm

**RNrd road, 8 bytes**

| off | type | meaning | tag |
|---|---|---|---|
| +00 | u8, u8 | 0, 1 in all 2155 records | [V] measured, meaning [?] |
| +02 | u16 | road length = sum of its segments' +04 / 4 (unit 4000/65535 m) | [V] measured ratio 0.247..0.250 on all roads; reader [?] |
| +04 | u8 | runtime gate id, set to 0xFF at load by 0x6D1230 `*(DAT_00d11fc8 + 4 + i*8) = 0xff`; read by 0x6D1290 / 0x6D23F0 | [V] |
| +05 | u8 | 16..255 (203, 67, 135, 101, 169 most common) | [?] |
| +06 | u8 | 0..101, 0 in 1377 roads | [?] |
| +07 | u8 | 0 | [V] measured |

**RNpf lane profile, 64 bytes** = 4-byte header + 15 lane words.

| off | type | meaning | tag / evidence |
|---|---|---|---|
| +00 | u8 | lane count (1..15) | [V] 0x884230 `*pbVar2 - iVar3 - 1`; RNlm counts match it on every segment |
| +01 | u8 | lanes in the first direction; the other direction has `[0]-[1]` | [V] 0x7E1E40 `uVar4 = pbVar5[1]` or `*pbVar5 - pbVar5[1]`; all `[1] <= [0]` |
| +02 | u16 | 0xAAAA padding | [V] all 1213 |
| +04 + 4i | u32 | lane i: bits 0..3 = lane type; bits 4..17 = width; bits 18..31 = signed lateral offset * 0.0122085 m (100/8191, float at 0xCCD454) | [V] type: 0x7E1F20 `& 0xf`; offset: 0x899BB0 `(float)(*(int *)(pbVar9 + iVar13 * 4 + 4) >> 0x12) * DAT_00ccd454`, negated for lanes of the first direction. Width field: [I] same unit (327 -> 3.99 m, the common value; spacing of offsets = 327) |
| unused lanes | | 0xFEBFFAFF fill | [V] measured |

Lane types seen (lane count): 1: 3562, 2: 256, 3: 1627 (narrow, e.g. 163 = 2 m shoulders), 4: 360, 7: 81, 9: 2,
10: 533. 0x884230 treats types 7 and 0xD specially (returns 1). What each type means (traffic lane, parking,
median, ...) is **[?]**.

**RNlm lane map, 32 bytes** [V] 0x87D500: two halves. `[0x00]` = lane count at node A, `[0x01..0x0F]` = for each
lane at A the lane index it continues into at B; `[0x10]` = lane count at node B, `[0x11..0x1F]` = the reverse map.
Read by direction (`bVar10` picks the half). All 765 maps stay below the other side's lane count. Max 15 lanes. Lane
maps are shared by many segments (index 5 by 2194 segments).

## 4. 0x7EBB00 and the network hash

- The FNV-1a 64 hash is **not** computed in 0x7EBB00. 0x6F1790 hashes the five tables itself: assembly
  0x6F1A67..0x6F1AE6 seeds a stack pair with 0xCBF29CE4_84222325 and calls 0x6D8EE0 (thiscall, ECX = &hash) on
  RNpf, RNnd, RNsg, RNrd, RNlm with byte sizes count*stride, then stores the result in 0xD11F18/1C. **[V]**
- It runs after the load-time clears 0x6EEC00, 0x6D1230, 0x6D10C0 and before 0x7EBB00. **[V]**
- 0x6D8EE0 = FNV-1a 64 byte loop: `hash = hash * 0x100000001B3; hash ^= byte`. **[V]**
- 0x7EBB00 = load-time pass over segments: clears flag 0x0008 on all, then repeats 0x7EBA50(seg, dir 0/1) until no
  segment changes. 0x7EBA50 toggles 0x0008 (and 0x0002 for direction 1) using lane-connectivity helpers
  0x7EA010 / 0x7EA080 (0x7E20D0, 0x7E5A40, 0x7E5B60). **[V]** structure, meaning **[?]** (likely lane dead-end
  propagation). Appended records go through it automatically.
- Users of 0xD11F18/1C: only 0x7EBEC0, which returns `roadHash XOR 0x7DFF90()` (XOR of three other subsystem
  hashes via vtable +0x14). **[V]**
- Callers of 0x7EBEC0: 0x761B20, 0x763F50, 0x765E70, 0x7660B0. 0x765E70 is `NetworkGame::SendHandshakeData`
  (source path string `...\Multiplayer\NetworkGame.cpp`), sends it over `DAT_00d7550c` vtable +0x1C channel 2;
  0x763F50 puts it in a 64-byte channel 1 message; 0x761B20 receives a 64-byte message and only acts when
  `FUN_007ebec0() == message hash`. **[V]** 0x7660B0 not read **[?]**.
- **Verdict**: the hash is a multiplayer peer-compatibility check. A changed road network makes this client's hash
  differ from stock peers, so their handshake / peer messages are ignored (multiplayer races with unmodified clients
  are expected to fail). No comparison with the server, a save file or another data file was found. Offline single
  player does not use it. **[V]** for the 3 functions read, **[I]** for "no other check" (only 4 callers exist).

## 5. 0x6D8EE0(ptr, size)

FNV-1a 64 update over `size` bytes into the 64-bit state at ECX. Nothing else. **[V]**

## 6. Fixed limits

| limit | value | where | tag |
|---|---|---|---|
| table counts | 65535 each | RNhd u16[7] (0x6F1790 `movzx`) | [V] |
| segments | **32767** | segment indices are sign-extended in many readers: 0x6EEC00 `*(short *)(puVar4 + 2) * 0x18`, 0x6E6FD0 `(short)uVar2 * 0x18`, 0x7DFC10 `(short)param_2 * 0x18`, 0x6D2940 `short param_2` | [V] |
| nodes | 32767 | node +0C compared as `short` in 0x6DF2C0 | [V] |
| roads | 32767; 0xFFFF = none | seg +06 read as `short`, `-1 < sVar1` (0x6D0F20) | [V] |
| profiles | 32767; < 0 = default | node +0E `short`, 0x6D13B0 | [V] |
| segments per node | 7 | node record layout, u8 count | [V] |
| lanes per profile / lane map | 15 | 64-byte profile, 16-byte RNlm halves | [V] |
| segment length | 1000 m | u16 * 1000/65535 | [V] |
| handle length | 500 m | u16 * 500/65535 | [V] |
| lane offset | +-100 m | signed 14 bits * 100/8191 | [V] |
| road length field | 4000 m | u16, sum/4 | [V] measured unit |
| GPS / path result | 510 segments (0x1FE) | 0x6EC030 `if ((int)local_10 < 0x1fe)`, else the path is dropped (nav type set back to 2) | [V] |
| grid list per cell | 255 entries | CGcn list counts are u8 (cell +0x08..+0x0B); 0x6EEC00 reads `*(byte *)(cell + 0xb)` for list 3 and each entry as u16 | [V] |
| A* work arrays | sized `roads*2 + 4` nodes (16 B each) + 2 bitsets of `(roads*2+11)/8` bytes | 0x6E6910 (`PathFinder`, `AStarSearchRequestSlotPool`) from 0xD11F70 at construction | [V] dynamic; construction time relative to the L5RA load [?] (no direct caller, created through a vtable) |
| 0xBC-byte runtime records | count from RNhd[3] = 0 in L5RA | 0x6F1790 `_DAT_00d11fec = x * 0xbc` | [?] use |

No static array sized for nodes or segments was found in the functions read; the tables are used in place from the
loaded chunk. **[V]** for 0x6F1790 (pointers into the chunk), **[I]** for the rest of the game.

## 7. How traffic and the GPS use the network

**GPS / path search (A*)** [V]
- Graph nodes are **roads x 2 directions** (+4 slots for start/goal partial segments): 0x6D0F20 returns
  `dir + 4 + road*2`, or `dir + 0 / +2` for the start / goal segment, -1 for anything else.
- Expansion 0x6EC030: from the current node it scans the node's segment list (+10/+12), filters each segment with
  0x6D92A0, looks through connector segments (flag 0x0001) to the road behind them, and walks a road to its far
  junction with 0x6DF2C0 (follows degree-2 nodes using only the first two list slots, adds `len * 1000/65535`, stops
  at degree > 2, fails at degree < 2 or on a loop).
- The result is written as a list of segment indices into nav +0x30C (count nav +0x308, max 510), nav type 3 =
  "follow path"; 0x6E6FD0 / 0x6D1660 follow it.
- No precomputed routing table exists in the CARP; everything is computed from RNnd/RNsg/RNrd at run time.

**Traffic** (partly read)
- Lane choice uses the profile at the entry node (0x7DFC10 picks node A or B profile and the flip bits
  0x200/0x400), the lane split `pf[1]`, lane types (0x884230 checks type 7 and 0xD) and lane offsets (0x899BB0);
  lane continuity across a segment comes from RNlm (0x87D500). **[V]**
- Next-segment choice when not on a path: 0x6E6FD0 (nav type 2) scans the node list, applies flags 0x40, 0x80,
  0x1000/0x2000, 0x8000/0x4, 0x2, 0x20 according to nav options +0x5D/+0x5E/+0x5F, looks ahead up to 0x13 segments or
  100 m. **[V]**
- Spatial lookup of nearby segments: list 3 of the collision-grid cells (0x6EEC00 reads it to find segments inside an
  area). **[V]** that the grid is the spatial index; that traffic spawning uses it too is **[I]**.
- The actual spawn routine (AITrafficManager, string 0xBCEEF8, code around 0x896510 / 0x898220) was **not** read.
  Which lane types or flags allow spawning is **[?]**.

## 8. Other data that indexes segments or nodes

- CARP rows in L5RA [V]: `CARP`, `CDat` (group of `CGrd`, `cl`, `co` = collision data), `Map ` (empty group),
  `RNgp` (group of the six RN rows). Only the RN rows hold road data; `cl` (one 10-byte record) and `CGrd` belong
  to the collision group. RNsg is the last data block in the chunk, the others are before it, so growing any
  table shifts the following rows' offsets and the chunk size.
- **Collision grid list 3** (chunk 0x3B802 `CGcn`, STREAML5RA_25000.BUN) holds segment indices per cell; a new
  segment must be added to every cell it crosses. **[V]** (stage B notes + 0x6EEC00)
- Area closure list 0xD5A1F8 holds rectangles, not segment indices (0x6EEC00 finds segments through the grid).
  Where it is loaded from is **[?]**.
- Race routes are stored at run time as segment-index lists (nav +0x30C) and marked with flags 0x8000/0x4; whether
  any shipped race / event data (VLT, gameplay database, server event XML) stores segment or node indices is **[?]**.
  Appending at the end keeps every existing index unchanged, so such data would stay valid.
- Multiplayer handshake hash, section 4.

## Verdict: appending a road joined to an existing node

Appending is feasible and the format is now known enough. Keep every existing index unchanged; only append and
patch the joined node. What the editor must write:

1. **RNnd**: new nodes with +0C = own index, +0E = a profile index, +10 = count, +11 = 0xAA, list at +12, 0xAA
   padding. On the existing node, append the new segment index to its list (count + 1 must stay <= 7).
2. **Where to join** [I]: join at a junction (degree >= 3) or a dead end (degree 1). Joining in the middle of a road
   (degree 2) turns that node into a junction while both sides keep the same road index. The road walk 0x6DF2C0
   would then stop mid-road, and the A* closed set (one slot per road and direction) would mix two different chains.
   If a mid-road join is needed, split that road: give one side a new RNrd record and re-index those segments' +06.
   Shipped junctions use connector segments (flag 0x0001, road 0xFFFF) between junction nodes; the plain case
   "new road segment directly at the junction node" also appears in the data (road ends are at degree 3..7 nodes).
3. **RNsg**: +00/+02 nodes, +04 = arc length * 65.535, +06 = new road index, +08 = own index, +0A = a lane map
   whose two counts equal the lane counts of the two end profiles, +0C = 0x0100 (curved) plus 0x0200/0x0400 if a
   profile is reversed, 0x0040 for one-way; leave runtime bits (0x4, 0x8, 0x80, 0x1000, 0x2000, 0x4000, 0x8000)
   at 0. +0E/+10 handle lengths and +12/+15 s8 directions as in section 1.
4. **RNrd**: one record per new chain: +00 = 00 01, +02 = sum of the segments' +04 / 4, +04 = 0xFF, +05/+06 copied
   from a similar road (meaning unknown), +07 = 0.
5. **RNpf / RNlm**: reuse existing profiles and lane maps where the lane layout fits, else append.
6. **RNhd** counts, CARP row sizes and offsets, chunk size.
7. **CGcn list 3** in every grid cell the new segments cross (<= 255 per cell).
8. Expect multiplayer handshakes with stock clients to fail (hash). Offline play is not affected.

Still unknown (must be checked in game before relying on them): which lane type / flag lets traffic spawn on a
segment, meaning of flags 0x0002 / 0x0010 / 0x0020 and road bytes +05 / +06, the 0xBC-byte runtime records, whether
the PathFinder arrays are built after the L5RA load (dynamic size), and whether any other shipped file stores
segment or node indices. Nothing here has been tested in the game.

## Traffic spawn and remaining unknowns

Follow-up pass, same day, same binary, REA only. Tags as above.

### 1. Traffic spawn

What was found:
- `AITrafficManager` constructor = 0x8A0230 (references the string 0xBCEEF8, vtables 0xBDDA4C / 0xBDDA34 / 0xBDDA18 /
  0xBDD9EC, registers a task with 0x775A90(9, 1.0, 0.5, 1)). **[V]**
- Spawn request queue = 0x890E20 (virtual, no direct callers). It checks two attribute flags (hashes 0x8D76590B for
  the traffic branch and 0x07ABE879 for the other branch, read with 0x6C4700), a live-car cap (`in_ECX[0xe] < 0xf`
  = at most 15 traffic cars; `in_ECX[0x3f] <= 0xe` for the other kind), then queues a 0x20-byte request built by
  0x87DCC0. **[V]** code; what the two attributes are called is **[?]**.
- Lane picking, used when a car is placed or changes lane:
  - 0x893930 scans the lanes of a segment and keeps the one whose lateral offset (lane word >> 18, times
    100/8191 m) is closest to the requested offset. It steps over lanes for which 0x884230 returns true. **[V]**
  - 0x884230 returns true when the lane type at either end (low nibble of the lane word) is **7 or 0xD**.
    0x889EE0 uses the same test to skip leading lanes. So lane types 7 and 13 are "not drivable". **[V]** code,
    **[I]** name. Shipped data has 81 lanes of type 7 and none of type 13.
- **Not found**: the routine that chooses *where* (which segment, how far from the player) a new car appears.
  Candidates read and ruled out: 0x89B300 (destructor), 0x89E7E0 (list insert), 0x6F2C00 / 0x6EBC90 (grid helpers),
  0x893930 / 0x889EE0 (lane pickers). It is reached only through virtual calls. **[?]**
- TRAFFIC_PATTERN (0xBC6690) is referenced only from a data table at 0xC9E464, not from code. **[V]** Its use is **[?]**.

What a new segment needs to be eligible, as far as the code read shows:
- A profile at each end with at least one lane per travel direction whose type is not 7 or 0xD. **[V]** 0x893930
  needs such a lane. An all-7/0xD direction gives no drivable lane.
- An RNlm map whose counts equal the two profiles' lane counts. **[V]** lane continuity 0x87D500.
- Flags 0x1000 / 0x2000 clear (no area closure) and the road gate 0x0080 not set. These are runtime bits and start
  at 0. **[V]** 0x6D92A0 / 0x6E6FD0.
- One-way 0x0040 is respected only in the entry direction. **[V]**
- Flags 0x0002 / 0x0010 / 0x0020: no traffic test on them was found. 0x0002 and 0x0020 are read only by the nav
  options +0x5E / +0x5F in 0x6E6FD0; who sets those options is **[?]**. 0x0010 is read only by race-route code
  0x6D9770. So: leave 0x0010 and 0x0020 at 0, and copy 0x0002 from a similar shipped road. **[I]**
- Road bytes +05 / +06: no reader was found in the traffic code read. **[?]** Copy them from a similar road.
- RNrd +00 is not "00 01" bytes. It is a **u16 8.8 fixed-point cost / length factor**, 0x0100 = 1.0 on every
  shipped road: 0x6A9160 reads `(float)(u16 << 8) * 1.5258789e-05` and multiplies the segment length by it. **[V]**
  Write 0x0100.
- Grid list 3: among the readers found, only 0x6EEC00 (area closures at load) reads it. A traffic reader of list 3
  was not found. **[?]** Writing list 3 stays required: closures need it, and other readers may exist.

**Exact test that would exclude a new segment** (from the code read): no drivable lane in the travel direction
(0x893930 / 0x884230), a set closure bit, the road gate, or the one-way rule. The spawn-location test itself is
**[?]**.

### 2. How a traffic car follows the road

- Next segment at a node: 0x6E6FD0 (nav type 2 = free driving).
  - Scans the node list (+10 / +12).
  - Skips connector segments for continuation (0x6D2940).
  - Applies one-way, gate, closure and race-route bits.
  - Scores candidates by direction against the car's heading, looking ahead up to 19 segments or 100 m.
  - Random pick among candidates under 2.2 m deviation when nav +0x60 is set.
  **[V]**
- Lane keeping: lane continuation through RNlm (0x87D500), lateral offset from the profile (0x899BB0 / 0x893930), and
  the profile flip bits 0x0200 / 0x0400 (0x7DFC10). **[V]**
- Curve: cubic Bezier from the segment handles (0x7E71C0). **[V]**
- A new segment written as in "Verdict" has every field these functions read. Nothing extra was found. **[I]** Not
  tested in game.

### 3. 0xBC-byte records (RNhd[3]) and RNhd[5]

- RNhd[3] goes to 0xD11F24. Its only xrefs are the two writes in 0x6F1790 (clear, then load); it feeds only the
  size sum 0xD11FEC = n * 0xBC. No table pointer is bound for it: 0xD11FC4 has no xrefs. **[V]**
- RNhd[5] (+0x0A) is never read: 0x6F1790 reads +0, +2, +4, +6, +8, +0x0C only. **[V]**
- So 0 / 0 for appended data is fine. **[V]**

### 4. PathFinder arrays: when built

- The getter is code at 0x6E6BF0 (not a Ghidra function; decoded from bytes):
  `if (!*(0xD12000)) *(0xD12000) = new(0xB4) PathFinder (0x6E6910); return *(0xD12000) + 0x24;`. **[V]** bytes
  `a1 00 20 d1 00 85 c0 75 31 ... 68 b4 00 00 00 e8 ... 8b c8 e8 ... a3 00 20 d1 00`.
- So it is created **lazily, on the first path request**. Its arrays are sized from the road count 0xD11F70 at that
  moment, not from a fixed number. **[V]**
- The destructor 0x6E6B10 frees the arrays and clears 0xD12000, so the next request rebuilds them. **[V]** Its
  caller (likely network unload) is **[?]**.
- Appended roads are counted as long as the first GPS request comes after the L5RA network has loaded, which the
  stock game also needs. 0x6EC030 bounds-checks the bitset index (`iVar6 < in_ECX[4]`), so a stale size would make
  the search fail, not corrupt memory. **[I]**

### 5. Other places with indices

- **Grid lists 1 / 2** (0x3B802): empty in the file. 0x6EBC90 adds and removes ids in a given list at run time
  (0x6EBB60 add, 0x6E5C50 remove). These are runtime lists for moving objects. **[V]** code, **[I]** that lists
  1/2 are those lists. Nothing to write.
- **L5RA chunk 0x8003B900** (right after 0x3B800): 697 subchunks 0x3B901, loaded by 0x706C20 / 0x706A60 into a
  sorted map keyed by the u32 at payload +0x10 (a hash). **[V]** loader. Content: hashes and s16 values that look like
  positions. A test for chains of connected segment indices at strides 2..16 found nothing above noise. **[I]** not
  road-index data. Purpose **[?]**.
- **Race routes at run time**: lists of segment indices (nav +0x30C) and an array of up to 512 u16 at 0x6A9160
  (`param_1 + 0x200` count). They are filled at run time. Their source data is **[?]**.
- Attrib vault, server event XML, save data: no code path from the road tables to them was found. Not searched in
  the files. **[?]**
- Appending keeps every existing index, so data that stores existing indices stays valid either way. **[V]**
  (append-only)

### Go / no-go

- **GPS: go.** A* uses only RNnd / RNsg / RNrd, with arrays sized at run time.
- **Traffic driving on the new road once it gets there: go.** Every field the follow code reads is known.
- **Traffic spawning on the new road: not proven.** The spawn-location routine was not found.
- Recommended next step: a runtime test in the game copy. Log the segment index where 0x87DCC0 requests are built,
  or watch nav +0x8E of traffic cars, and see whether new segment indices appear.
