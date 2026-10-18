#include "bowl/app/dev_link.hpp"

#include "bowl/core/file.hpp"
#include "bowl/core/log.hpp"
#include "bowl/core/profiler.hpp"
#include "bowl/core/time.hpp"
#include "bowl/ecs/hierarchy.hpp"

#include <algorithm>
#include <cctype>
#include <random>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace bowl::devlink {

namespace fs = std::filesystem;

namespace {

constexpr u32 link_protocol = 0x4B4C4442; // "BDLK": hosts with other numbers ignore it
constexpr f64 frame_every = 0.1, schedules_every = 0.5, detail_every = 0.25;
constexpr usize page_budget = 40 * 1024;  // bytes of names per EntityPage, well under a reliable message
constexpr usize component_budget = 16 * 1024;
constexpr usize frames_kept = 300;

i64 this_pid() {
#if defined(_WIN32)
    return static_cast<i64>(GetCurrentProcessId());
#else
    return static_cast<i64>(getpid());
#endif
}

bool process_alive(i64 pid) {
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    DWORD code = 0;
    bool alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
#else
    return kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

std::string random_key() {
    std::random_device device; // the OS's random source: the key only has to be unguessable to other programs
    static const char* hex = "0123456789abcdef";
    std::string key;
    for (int i = 0; i < 32; ++i) {
        u32 byte = device() & 0xFF;
        key += hex[byte >> 4];
        key += hex[byte & 15];
    }
    return key;
}

net::HostSettings settings(const std::string& key, bool server) {
    net::HostSettings s;
    s.local_only = true;
    s.protocol = link_protocol;
    s.secret = key;
    s.max_peers = server ? 4 : 0;
    return s;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// Whether a component's fields hold entity references: shown, but not edited, since its JSON has them as nothing.
class FindsEntities final : public reflect::FieldVisitor {
public:
    bool found = false;
    void boolean(std::string_view, bool&) override {}
    void integer(std::string_view, i64&, i64, i64) override {}
    void real(std::string_view, f64&) override {}
    void text(std::string_view, std::string&) override {}
    void vector(std::string_view, f32*, int) override {}
    void color(std::string_view, Color&) override {}
    void entity(std::string_view, u64) override { found = true; }
    bool begin_group(std::string_view) override { return true; }
    void end_group() override {}
    bool begin_list(std::string_view, usize&, bool) override { return true; }
    bool asset(std::string_view, std::string&, std::string_view) override { return false; }
    void unsupported(std::string_view, std::string_view) override {}
};

template <class T> std::string encoded_text(const T& value) {
    serial::Context ctx;
    return json::stringify(serial::to_json(value, ctx), 0);
}

} // namespace

fs::path folder() { return fs::temp_directory_path() / "bowl-devlink"; }

std::vector<GameEntry> running_games() {
    std::vector<GameEntry> games;
    std::error_code ec;
    for (const auto& item : fs::directory_iterator(folder(), ec)) {
        if (item.path().extension() != ".json") continue;
        auto text = read_file_text(item.path());
        auto parsed = text ? json::parse(*text) : json::ParseResult{};
        if (!parsed) continue;
        GameEntry g;
        g.game = std::string(parsed.value["game"].as_string());
        g.port = static_cast<u16>(parsed.value["port"].as_int());
        g.key = std::string(parsed.value["key"].as_string());
        g.pid = parsed.value["pid"].as_int();
        g.file = item.path();
        if (!process_alive(g.pid)) { // it exited without tidying up (a crash, a debugger stop)
            fs::remove(item.path(), ec);
            continue;
        }
        if (g.port && !g.key.empty()) games.push_back(std::move(g));
    }
    std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) { return a.pid < b.pid; });
    return games;
}

// The game's side.

DevLink::DevLink(Sources sources) : sources_(std::move(sources)), key_(random_key()) {
    messages_.on<ListEntities>([this](net::PeerId from, const ListEntities& m) { send_page(from, m); });
    messages_.on<Watch>([this](net::PeerId from, const Watch& m) {
        if (m.generation == 0) watching_.erase(from);
        else watching_[from] = {ecs::Entity{m.index, m.generation}, -1e9};
    });
    messages_.on<SetComponent>([this](net::PeerId, const SetComponent& m) {
        ecs::World* world = sources_.world;
        ecs::Entity e{m.index, m.generation};
        auto id = ecs::find_component(m.component);
        if (!world || !world->alive(e) || !id || world->locked()) return;
        const ecs::ComponentInfo& info = ecs::component_info(*id);
        void* ptr = world->component_ptr(e, *id);
        auto parsed = json::parse(m.json);
        if (!ptr || !info.load || !parsed) {
            warn("dev link: can't change {}: {}", m.component, parsed ? "it isn't savable" : parsed.error->message);
            return;
        }
        serial::Context ctx{sources_.assets};
        if (!info.load(parsed.value, ptr, ctx)) warn("dev link: {} changed with errors", m.component);
        for (const auto& w : ctx.warnings) warn("  {}", w);
        if (auto it = std::find_if(watching_.begin(), watching_.end(), [&](auto& p) { return p.second.entity == e; });
            it != watching_.end())
            it->second.last = -1e9; // show the result right away
    });
    messages_.on<SetSystemEnabled>([this](net::PeerId, const SetSystemEnabled& m) {
        ecs::Schedule* s = m.schedule == "fixed" ? sources_.fixed : m.schedule == "frame" ? sources_.frame : nullptr;
        if (s) s->set_enabled(m.system, m.enabled);
        last_schedules_check_ = -1e9; // the editor sees the switch at once
    });
    messages_.on<TimeControl>([this](net::PeerId, const TimeControl& m) {
        if (sources_.time) sources_.time(m);
    });
}

DevLink::~DevLink() {
    host_.stop();
    if (!announced_.empty()) {
        std::error_code ec;
        fs::remove(announced_, ec);
    }
}

bool DevLink::start(bool announce) {
    if (!host_.start(settings(key_, true))) return false;
    if (!announce) return true;
    std::error_code ec;
    fs::create_directories(folder(), ec);
    json::Value doc;
    doc.set("game", sources_.game);
    doc.set("port", static_cast<f64>(host_.port()));
    doc.set("key", key_);
    doc.set("pid", static_cast<f64>(this_pid()));
    std::string text = json::stringify(doc);
    fs::path file = folder() / (std::to_string(this_pid()) + ".json");
    if (!write_file_bytes(file, text.data(), text.size())) {
        warn("dev link: can't write {}; Bowl Editor won't find this game", file.string());
        return true;
    }
    // Only this user may read the key (the temp folder may be shared, as on Linux).
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
    announced_ = file;
    info("dev link: Bowl Editor can attach to this game (port {})", host_.port());
    return true;
}

void DevLink::update(f64 now) {
    now_ = now >= 0 ? now : now_seconds();
    host_.update(now_);
    while (auto e = host_.receive()) {
        if (e->type == net::Event::Type::Connected) {
            messages_.send(host_, e->peer, Hello{sources_.game});
            send_schedules(e->peer);
        } else if (e->type == net::Event::Type::Disconnected) {
            watching_.erase(e->peer);
        } else if (e->type == net::Event::Type::Message) {
            messages_.handle(*e);
        }
    }
    std::vector<net::PeerId> peers = host_.peers();
    if (peers.empty()) return;

    if (now_ - last_schedules_check_ >= schedules_every) {
        last_schedules_check_ = now_;
        send_schedules(0);
    }
    if (now_ - last_frame_sent_ >= frame_every) {
        last_frame_sent_ = now_;
        Frame f = sources_.stats ? sources_.stats() : Frame{};
        if (sources_.time_state) {
            TimeControl t = sources_.time_state();
            f.paused = t.paused;
            f.time_scale = t.time_scale;
        }
        for (const auto& z : profiler::last_frame()) f.zones.push_back({std::string(z.name), z.milliseconds, z.calls});
        for (net::PeerId p : peers) messages_.send(host_, p, f);
    }
    for (auto& [peer, w] : watching_)
        if (now_ - w.last >= detail_every) {
            w.last = now_;
            send_detail(peer, w.entity);
        }
}

void DevLink::send_schedules(net::PeerId peer) {
    Schedules s;
    if (sources_.fixed) s.fixed = sources_.fixed->describe();
    if (sources_.frame) s.frame = sources_.frame->describe();
    // Changes worth sending: which systems, their access, batches and switches; not their timings, which Frame has.
    Schedules shape = s;
    for (auto* list : {&shape.fixed, &shape.frame})
        for (auto& info : *list) info.ms = 0;
    std::string now = encoded_text(shape);
    if (peer != 0) { // a newcomer gets it whatever it is
        messages_.send(host_, peer, s);
        return;
    }
    if (now == schedules_sent_) return;
    schedules_sent_ = std::move(now);
    for (net::PeerId p : host_.peers()) messages_.send(host_, p, s);
}

void DevLink::send_page(net::PeerId peer, const ListEntities& request) {
    EntityPage page;
    page.from = request.from;
    ecs::World* world = sources_.world;
    if (!world) {
        messages_.send(host_, peer, page);
        return;
    }
    std::vector<ecs::Entity> all;
    for (const auto& a : world->archetypes()) all.insert(all.end(), a->entities().begin(), a->entities().end());
    std::sort(all.begin(), all.end(), [](ecs::Entity x, ecs::Entity y) { return x.index < y.index; });
    std::string filter = lower(request.filter);
    usize budget = page_budget, skipped = 0;
    u32 count = std::min<u32>(request.count, 2000);
    for (ecs::Entity e : all) {
        EntityRow row{e.index, e.generation, {}, {}};
        if (const auto* n = world->try_get<ecs::Name>(e)) row.name = n->value;
        for (ecs::ComponentId c : world->components_of(e)) row.components.emplace_back(ecs::component_info(c).name);
        if (!filter.empty()) {
            bool match = lower(row.name).find(filter) != std::string::npos;
            for (const auto& c : row.components) match = match || lower(c).find(filter) != std::string::npos;
            if (!match) continue;
        }
        ++page.total;
        if (skipped < request.from) {
            ++skipped;
            continue;
        }
        if (page.rows.size() >= count) continue;
        usize size = 16 + row.name.size();
        for (const auto& c : row.components) size += c.size() + 2;
        if (size > budget) {
            count = static_cast<u32>(page.rows.size()); // the page is full: the rest come with the next request
            continue;
        }
        budget -= size;
        page.rows.push_back(std::move(row));
    }
    messages_.send(host_, peer, page);
}

void DevLink::send_detail(net::PeerId peer, ecs::Entity e) {
    EntityDetail d{e.index, e.generation, false, {}};
    ecs::World* world = sources_.world;
    if (world && world->alive(e)) {
        d.alive = true;
        serial::Context ctx{sources_.assets};
        for (ecs::ComponentId c : world->components_of(e)) {
            const ecs::ComponentInfo& info = ecs::component_info(c);
            ComponentJson cj{std::string(info.name), {}, false};
            void* ptr = world->component_ptr(e, c);
            if (info.save) {
                bool refs = false;
                if (info.visit && ptr) {
                    FindsEntities finder;
                    info.visit(ptr, finder);
                    refs = finder.found;
                }
                std::string text = json::stringify(info.save(ptr, ctx), 2);
                if (text.size() <= component_budget) {
                    cj.json = std::move(text);
                    cj.editable = !refs && info.size > 0;
                }
            }
            d.components.push_back(std::move(cj));
        }
    }
    messages_.send(host_, peer, d);
}

// The editor's side.

DevLinkClient::DevLinkClient() {
    messages_.on<Hello>([this](net::PeerId, const Hello& m) { hello_ = m; });
    messages_.on<Schedules>([this](net::PeerId, const Schedules& m) { schedules_ = m; });
    messages_.on<Frame>([this](net::PeerId, const Frame& m) {
        frames_.push_back(m);
        while (frames_.size() > frames_kept) frames_.pop_front();
    });
    messages_.on<EntityPage>([this](net::PeerId, const EntityPage& m) { page_ = m; });
    messages_.on<EntityDetail>([this](net::PeerId, const EntityDetail& m) { detail_ = m; });
}

DevLinkClient::~DevLinkClient() { disconnect(); }

bool DevLinkClient::connect(const GameEntry& game) { return connect(game.port, game.key); }

bool DevLinkClient::connect(u16 port, const std::string& key) {
    disconnect();
    if (!host_.start(settings(key, false))) return false;
    peer_ = host_.connect(net::Address::loopback(port));
    return peer_ != 0;
}

void DevLinkClient::disconnect() {
    host_.stop();
    peer_ = 0;
    connected_ = false;
    hello_.reset();
    schedules_.reset();
    frames_.clear();
    page_.reset();
    detail_.reset();
}

void DevLinkClient::update(f64 now) {
    if (!host_.running()) return;
    host_.update(now);
    while (auto e = host_.receive()) {
        if (e->peer != peer_) continue;
        if (e->type == net::Event::Type::Connected) connected_ = true;
        else if (e->type == net::Event::Type::Disconnected) {
            host_.stop();
            peer_ = 0;
            connected_ = false;
        } else messages_.handle(*e);
    }
}

template <class T> void DevLinkClient::send(const T& message) {
    if (connected_) messages_.send(host_, peer_, message);
}

void DevLinkClient::list_entities(const ListEntities& request) { send(request); }
void DevLinkClient::watch(u32 index, u32 generation) {
    if (generation == 0 || !detail_ || detail_->index != index || detail_->generation != generation) detail_.reset();
    send(Watch{index, generation});
}
void DevLinkClient::set_component(const SetComponent& change) { send(change); }
void DevLinkClient::set_system_enabled(const SetSystemEnabled& change) { send(change); }
void DevLinkClient::time(const TimeControl& control) { send(control); }

} // namespace bowl::devlink
