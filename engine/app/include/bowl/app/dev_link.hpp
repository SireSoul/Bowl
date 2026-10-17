#pragma once

#include "bowl/ecs/schedule.hpp"
#include "bowl/net/host.hpp"
#include "bowl/net/replication.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// The dev link: Bowl Editor's Game tab attached to a running game, on the same computer. A dev build's game listens on
// the loopback address only, with a random key in a file only this user can read (<temp>/bowl-devlink/<pid>.json), so
// nothing on a network can reach it and only someone who can read the file can connect. Through it the editor sees the
// profiler, the schedules (which system reads and writes what, and why each batch starts) and the entities, and can
// change components, switch systems off and pause, step or slow the game.
//
// The game's side is DevLink, which the engine runs in dev builds (AppConfig::dev_link); the editor's is DevLinkClient.
namespace bowl::devlink {

inline constexpr u32 protocol_version = 1;

// Game to editor.
struct Hello {
    std::string game;
    u32 version = protocol_version;
};
BOWL_REFLECT(Hello, game, version)

struct Schedules {
    std::vector<ecs::SystemInfo> fixed, frame;
};
BOWL_REFLECT(Schedules, fixed, frame)

struct Zone {
    std::string name;
    f64 ms = 0; // this frame, all calls and threads
    u32 calls = 0;
};
BOWL_REFLECT(Zone, name, ms, calls)

struct Frame {
    f32 ms = 0; // CPU time of the last frame
    u32 entities = 0, archetypes = 0, voices = 0, sprites = 0, draw_calls = 0;
    bool paused = false;
    f32 time_scale = 1;
    std::vector<Zone> zones;
};
BOWL_REFLECT(Frame, ms, entities, archetypes, voices, sprites, draw_calls, paused, time_scale, zones)

struct EntityRow {
    u32 index = 0, generation = 0; // the entity, in two halves (JSON numbers hold 53 bits)
    std::string name;              // its ecs::Name, if any
    std::vector<std::string> components;
};
BOWL_REFLECT(EntityRow, index, generation, name, components)

struct EntityPage {
    u32 from = 0, total = 0; // total: entities matching the filter
    std::vector<EntityRow> rows;
};
BOWL_REFLECT(EntityPage, from, total, rows)

struct ComponentJson {
    std::string name;
    std::string json;      // as a save writes it; empty if it can't be shown
    bool editable = false; // savable, and small enough to show
};
BOWL_REFLECT(ComponentJson, name, json, editable)

struct EntityDetail {
    u32 index = 0, generation = 0;
    bool alive = false;
    std::vector<ComponentJson> components;
};
BOWL_REFLECT(EntityDetail, index, generation, alive, components)

// Editor to game.
struct ListEntities {
    u32 from = 0, count = 200;
    std::string filter; // part of the name or a component name; empty: all
};
BOWL_REFLECT(ListEntities, from, count, filter)

struct Watch { // the game sends this entity's EntityDetail a few times a second; generation 0: none
    u32 index = 0, generation = 0;
};
BOWL_REFLECT(Watch, index, generation)

struct SetComponent {
    u32 index = 0, generation = 0;
    std::string component, json;
};
BOWL_REFLECT(SetComponent, index, generation, component, json)

struct SetSystemEnabled {
    std::string schedule; // "fixed" or "frame"
    std::string system;
    bool enabled = true;
};
BOWL_REFLECT(SetSystemEnabled, schedule, system, enabled)

struct TimeControl {
    bool paused = false;
    bool step = false; // one fixed tick while paused
    f32 time_scale = 1;
};
BOWL_REFLECT(TimeControl, paused, step, time_scale)

// A running game, from its file in the dev link folder.
struct GameEntry {
    std::string game;
    u16 port = 0;
    std::string key;
    i64 pid = 0;
    std::filesystem::path file;
};

// The folder games announce themselves in: <temp>/bowl-devlink.
std::filesystem::path folder();
// The games running now. Files left by games that have exited are removed.
std::vector<GameEntry> running_games();

// What the game's side reads and drives. Plain callbacks, so it runs without a window (tests).
struct Sources {
    std::string game;
    ecs::World* world = nullptr;
    AssetManager* assets = nullptr; // for components with Asset fields
    ecs::Schedule* fixed = nullptr;
    ecs::Schedule* frame = nullptr;
    std::function<Frame()> stats;                       // the frame's numbers (zones are added by DevLink)
    std::function<void(const TimeControl&)> time;       // pause, step, time scale
    std::function<TimeControl()> time_state;
};

// The game's side: listens, answers and streams. Call update() once a frame, outside the schedules.
class DevLink : NonCopyable {
public:
    explicit DevLink(Sources sources);
    ~DevLink();

    // Starts listening and writes the announcement file (announce = false: no file, for tests). False if the socket
    // can't open.
    bool start(bool announce = true);
    void update(f64 now = -1);
    u16 port() const { return host_.port(); }
    const std::string& key() const { return key_; }
    usize connections() const { return host_.peers().size(); }

private:
    void send_schedules(net::PeerId peer); // to a newcomer; 0: to everyone, if it changed
    void send_page(net::PeerId peer, const ListEntities& request);
    void send_detail(net::PeerId peer, ecs::Entity e);
    Sources sources_;
    net::Host host_;
    net::Messages messages_;
    std::string key_;
    std::filesystem::path announced_;
    std::string schedules_sent_; // what the last Schedules said, to send it again only when it changes
    struct Watching {
        ecs::Entity entity;
        f64 last = -1e9;
    };
    std::unordered_map<net::PeerId, Watching> watching_;
    f64 now_ = 0, last_frame_sent_ = -1e9, last_schedules_check_ = -1e9;
};

// The editor's side: connects to a running game and keeps what it last heard.
class DevLinkClient : NonCopyable {
public:
    DevLinkClient();
    ~DevLinkClient();

    bool connect(const GameEntry& game);
    bool connect(u16 port, const std::string& key); // to a game on this computer
    void disconnect();
    bool connected() const { return connected_; }
    bool connecting() const { return peer_ != 0 && !connected_; }
    void update(f64 now = -1);

    // What the game last said.
    const std::optional<Hello>& hello() const { return hello_; }
    const std::optional<Schedules>& schedules() const { return schedules_; }
    const std::deque<Frame>& frames() const { return frames_; } // the last few seconds, oldest first
    const std::optional<EntityPage>& page() const { return page_; }
    const std::optional<EntityDetail>& detail() const { return detail_; }

    void list_entities(const ListEntities& request);
    void watch(u32 index, u32 generation);
    void set_component(const SetComponent& change);
    void set_system_enabled(const SetSystemEnabled& change);
    void time(const TimeControl& control);

private:
    template <class T> void send(const T& message);
    net::Host host_;
    net::Messages messages_;
    net::PeerId peer_ = 0;
    bool connected_ = false;
    std::optional<Hello> hello_;
    std::optional<Schedules> schedules_;
    std::deque<Frame> frames_;
    std::optional<EntityPage> page_;
    std::optional<EntityDetail> detail_;
};

} // namespace bowl::devlink
