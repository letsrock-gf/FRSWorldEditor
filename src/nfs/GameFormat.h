#pragma once

// Which game's files are open.
//
// NFS World keeps the chunk ids of Underground 2 but lays the records out
// differently, and ships each streaming section as its own file instead of a
// range of one stream. The readers branch on this; it is set when a region is
// opened (World::Open), from the region's name.
namespace nfs {

// Underground 1 is told from Underground 2 by the World that opened it
// (World::IsUnderground1); NFS World by ActiveGame, below.
enum class Game { Underground2, Underground1, World };

inline Game& ActiveGame() {
    static Game game = Game::Underground2;
    return game;
}

inline bool IsWorld() { return ActiveGame() == Game::World; }

} // namespace nfs
