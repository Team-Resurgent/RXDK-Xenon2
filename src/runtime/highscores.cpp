// The high-score table, kept between sessions as the WHDLoad install does:
// its 190 bytes at $3C33E restored before the game starts, and saved once
// they have changed and settled (the name entered).
#include "runtime/highscores.h"

#include "runtime/platform.h"

#include <cstring>
#include <vector>

namespace {

constexpr u32 kTable = 0x3C33E, kTableBytes = 0xBE;
constexpr int kSettleFrames = 100;      // 2 s unchanged: the entry is done

u8 saved[kTableBytes];                  // as last saved (or loaded)
u8 seen[kTableBytes];                   // as last frame
int settled;                            // frames the table has been as it is

} // namespace

void highscores_load() {
    std::vector<uint8_t> data;
    if (platform_save_read("highs", data) && data.size() == kTableBytes) {
        std::memcpy(MEM + kTable, data.data(), kTableBytes);
        platform_log("highscores: loaded (%.15s)\n", reinterpret_cast<const char *>(MEM + kTable + 0x28));
    }
    std::memcpy(saved, MEM + kTable, kTableBytes);
    std::memcpy(seen, saved, kTableBytes);
}

void highscores_frame() {
    const u8 *table = MEM + kTable;
    if (std::memcmp(table, seen, kTableBytes)) {
        std::memcpy(seen, table, kTableBytes);
        settled = 0;
        return;
    }
    if (++settled == kSettleFrames && std::memcmp(table, saved, kTableBytes)) {
        std::memcpy(saved, table, kTableBytes);
        if (platform_save_write("highs", saved, kTableBytes)) platform_log("highscores: saved\n");
        else platform_log("highscores: can't save\n");
    }
}
