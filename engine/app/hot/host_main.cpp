// The executable of a hot-reloadable game: the whole engine, plus a Game that forwards to the game library and
// reloads it whenever it's rebuilt. bowl_add_game passes in where the library is, which folders hold its sources, and
// how to rebuild it.

#include "bowl/app/hot_reload.hpp"
#include "bowl/app/main.hpp"

#include <string_view>

namespace {

bowl::HotReloadHost::Options options() {
    bowl::HotReloadHost::Options o;
    o.library = BOWL_HOT_RELOAD_LIBRARY_PATH;
    std::string_view dirs = BOWL_HOT_RELOAD_SOURCE_DIRS; // separated by '|'
    while (!dirs.empty()) {
        auto bar = dirs.find('|');
        if (bar != 0) o.source_dirs.emplace_back(dirs.substr(0, bar));
        dirs = bar == std::string_view::npos ? std::string_view{} : dirs.substr(bar + 1);
    }
    o.build_command = {BOWL_HOT_RELOAD_CMAKE, "--build", BOWL_HOT_RELOAD_BUILD_DIR, "--target", BOWL_HOT_RELOAD_TARGET,
                       "--config", BOWL_HOT_RELOAD_CONFIG};
    return o;
}

class HostedGame final : public bowl::HotReloadHost {
public:
    HostedGame() : HotReloadHost(options()) {}
};

} // namespace

BOWL_MAIN(HostedGame)
