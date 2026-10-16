// The executable of a hot-reloadable game: the whole engine, plus a Game that forwards to the game library and
// reloads it whenever it's rebuilt. ph_add_game passes in where the library is, which folders hold its sources, and
// how to rebuild it.

#include "ph/app/hot_reload.hpp"
#include "ph/app/main.hpp"

#include <string_view>

namespace {

ph::HotReloadHost::Options options() {
    ph::HotReloadHost::Options o;
    o.library = PH_HOT_RELOAD_LIBRARY_PATH;
    std::string_view dirs = PH_HOT_RELOAD_SOURCE_DIRS; // separated by '|'
    while (!dirs.empty()) {
        auto bar = dirs.find('|');
        if (bar != 0) o.source_dirs.emplace_back(dirs.substr(0, bar));
        dirs = bar == std::string_view::npos ? std::string_view{} : dirs.substr(bar + 1);
    }
    o.build_command = {PH_HOT_RELOAD_CMAKE, "--build", PH_HOT_RELOAD_BUILD_DIR, "--target", PH_HOT_RELOAD_TARGET,
                       "--config", PH_HOT_RELOAD_CONFIG};
    return o;
}

class HostedGame final : public ph::HotReloadHost {
public:
    HostedGame() : HotReloadHost(options()) {}
};

} // namespace

PH_MAIN(HostedGame)
