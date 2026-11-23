# NFS World road network (traffic, AI, GPS): stage B notes

Research notes, 21 Nov 2026. nfsw.exe 32-bit, MD5 4c32936debffcbdc208d39502b79a56f, static addresses (base 0x400000).
Tags: **[V]** verified (code read with REA and/or measured on the shipped files), **[I]** inferred, **[?]** unknown.

## Where it is

- **L5RA.BUN chunk 0x3B800** (622 KB in L5RA), payload 16-aligned. **[V]** `bChunkLoader(0x3B800 and 0x3B802,
  load 0x6F5EE0, unload 0x6F4870)` registered at 0xB09B50. 0x6F5E30: 0x3B800 -> 0x752EE0 (CARP row table) ->
  0x6F3BF0 (collision manager: CDat/CGrd, 0x6F0E80) and 0x6F4890; 0x3B802 = the per-section collision grid.
- The payload is a 'CARP' row table, same 16-byte rows as the collision packs ({tag, size << 8 | kind, count,
  offset}, kind bit 1 = offset from the row). Rows in L5RA: groups CARP, CDat, Map, RNgp; data CGrd (40 B),
  cl (10 B), co, **RNhd** (14 B), **RNlm** (765 x 32), **RNnd** (7521 x 32), **RNpf** (1213 x 64),
  **RNrd** (2155 x 8), **RNsg** (10898 x 24). `tools/nfsw/road_network.py` reads them. **[V]**
- 0x6F1790 (WRoadNetwork init, string "WRoadNetwork" 0xBC2DB8) reads RNhd as u16[7] = {profiles, nodes,
  segments, x (0 in L5RA; runtime 188-byte records), roads, ?, RNlm count} and binds
  RNpf -> 0xD11FB8, RNnd -> 0xD11FBC, RNsg -> 0xD11FC0, RNrd -> 0xD11FC8, RNlm -> 0xD11FCC; strides 64, 32, 24,
  8, 32. Every row count and size matches RNhd in L5RA. Node bounds are taken from node floats +0/+4/+8, padded
  50 m. It then seeds an FNV-1a 64 basis (0xD11F18/1C) and calls 0x7EBB00. **[V]**
- Nodes are in collision axes: (x = -game y, up, z = game x). Node 0 = (-759.9, 48.1, 4214.6). **[V]** (values in
  the map's range)
- **Spatial index: list 3 of every collision-grid cell** (chunk 0x3B802, 'CGcn', in STREAML5RA_25000.BUN) holds
  road segment indices: 2883 cells, 23490 entries, range 0..0x2A91 (= segments - 1). Lists 1 and 2 are empty in
  L5RA. `tools/nfsw/grid_lists.py`. **[V]**

## Field layouts, limits, hash

Read with REA in `nfsw-road-network-fields.md` (same folder). Spot-checked on every shipped record: segment +08 =
own index, segment +0A < 765 (RNlm), node +0C = own index, every node's segment list points back at it, max 7
segments per node, RNrd +00 = 0x0100 on all 2155 roads. **[V]**

## The author's road (21 Nov 2026) and the shipped graph

- Shipped degree-2 nodes: 3211, none between two different roads. **[V]**
- The pair rule "same reversed bit when two segments leave a node in opposite directions" holds for 13702 pairs and
  fails for 10576: the 0x200/0x400 bits are not a plain heading test. **[V]** measured; their rule **[?]**
- The author's road (game x 7984..7497, y -2729..-2763) is 396 m from the nearest graph node: node 7519, game
  (8305.4, -2497.0, 105.0), the dead end of road 2153 (segment 10895). Nothing of the graph lies nearer. **[V]**
- Common 3+3-lane profile: RNpf 10 (types 3,1,1,1,1,3, width 327 = 3.99 m, offsets ±163/491/819), 1124 nodes.
  Identity 6->6 lane map: RNlm 1. **[V]**

## Records seen (first look, superseded by the fields file)

- Node 32 B: f32 x, up, z; u32 (= node index in the first records); u8 segment count; u8 0xAA; u16 segment
  indices (0xAA padding). **[I]**
- Segment 24 B: u16 node a, u16 node b, u16, u16, u16 (= road index in the first records), u16 0, u16 flags
  (0x0100, 0x0110), then bytes that look like packed tangents. **[I]**
- Road 8 B, profile 64 B (u8 lane count then lane pairs), RNlm 32 B (lane maps). **[?]**
