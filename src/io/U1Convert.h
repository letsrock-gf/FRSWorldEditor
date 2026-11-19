#pragma once

#include "core/Types.h"

#include <string>
#include <vector>

// Need for Speed Underground's models in Underground 2's layout.
//
// The two games store a model the same way -- the object chunk 0x80134010
// with its header, texture list and mesh (docs/NFSU2-FORMATS.md, the last
// section) -- apart from three headers that grew and one vertex layout that
// went away:
//
//   0x00134011 object header  176 -> 192 bytes: version 0x13 -> 0x16, and
//              sixteen bytes before the name: the box's volume, triangles per
//              unit of that volume (what the LOD choice reads), then zero.
//              +0x88/+0x8C hold one value throughout each game's files.
//   0x00134900 mesh header     64 ->  68 bytes, the four new ones zero.
//   0x00134B01 vertices: the crowd's skinned 60-byte vertices (mesh flags
//              0x4081) have no counterpart in Underground 2's world; they
//              become the 24-byte {position, colour, uv} with flags 0.
//
// Every aligned piece is laid out again for its new place: headers and
// tables on 16 bytes, vertices on 128, as the game's loader expects them.

// `object` is a whole 0x80134010 chunk of Underground 1 that sat at
// `fileOffset` in its file. `out` gets the same model as Underground 2 stores
// it, laid out to start on a 128-byte boundary.
bool ConvertU1Object(ByteSpan object, u64 fileOffset, std::vector<u8>& out, std::wstring& error);
