// The executable of a hot-reloadable game: the whole engine, plus a Game that forwards to the game library
// (PH_HOT_RELOAD_LIBRARY_PATH, set by ph_add_game) and reloads it whenever it's rebuilt.

#include "ph/app/hot_reload.hpp"
#include "ph/app/main.hpp"

namespace {
class HostedGame final : public ph::HotReloadHost {
public:
    HostedGame() : HotReloadHost(PH_HOT_RELOAD_LIBRARY_PATH) {}
};
} // namespace

PH_MAIN(HostedGame)
