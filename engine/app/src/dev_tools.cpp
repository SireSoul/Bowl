#include "dev_tools.hpp"

#include <SDL3/SDL.h>

#if PH_DEV_TOOLS

#include "ph/app/inspector.hpp"
#include "ph/core/file.hpp"
#include "ph/core/profiler.hpp"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#include <algorithm>
#include <format>

namespace ph {

DevTools::DevTools(Engine& engine) : engine_(engine) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.IniFilename = "devtools.ini";
    ImGui::StyleColorsDark();
    f32 scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0) scale = 1;
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    style.WindowRounding = 6;
    style.FrameRounding = 4;
    if (auto font = engine.assets().resolve("engine/fonts/Roboto-Medium.ttf"))
        io.Fonts->AddFontFromFileTTF(font->string().c_str(), 15.0f);

    ImGui_ImplSDL3_InitForSDLGPU(engine.window().handle());
    ImGui_ImplSDLGPU3_InitInfo init{};
    init.Device = engine.gpu().handle();
    init.ColorTargetFormat = engine.gpu().swapchain_format();
    init.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    ImGui_ImplSDLGPU3_Init(&init);

    engine.renderer().set_overlay({
        [this](SDL_GPUCommandBuffer* cmd) {
            if (frame_open_) ImGui_ImplSDLGPU3_PrepareDrawData(ImGui::GetDrawData(), cmd);
        },
        [this](SDL_GPUCommandBuffer* cmd, SDL_GPURenderPass* pass) {
            if (frame_open_) ImGui_ImplSDLGPU3_RenderDrawData(ImGui::GetDrawData(), cmd, pass);
        },
    });
    info("dev tools: press F1 for the overlay");
}

DevTools::~DevTools() {
    engine_.renderer().set_overlay({});
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
}

void DevTools::handle_event(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_F1 && !event.key.repeat) visible_ = !visible_;
    ImGui_ImplSDL3_ProcessEvent(&event);
}

void DevTools::begin_frame() {
    frame_open_ = visible_;
    if (!frame_open_) {
        engine_.input().set_blocked(false, false);
        return;
    }
    ImGui_ImplSDLGPU3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    engine_.input().set_blocked(io.WantCaptureKeyboard, io.WantCaptureMouse);
}

void DevTools::end_frame(Game& game) {
    if (!frame_open_) return;
    draw_main();
    draw_inspector();
    game.dev_ui(engine_);
    engine_.scenes().dev_ui(engine_);
    ImGui::Render();
}

void DevTools::draw_main() {
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({420, 560}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Bowl")) {
        ImGui::End();
        return;
    }
    const auto& history = profiler::frame_history();
    f32 avg = 0;
    for (f32 ms : history) avg += ms;
    avg = history.empty() ? 0 : avg / static_cast<f32>(history.size());
    ImGui::Text("%.1f fps  %.2f ms (cpu)", avg > 0 ? 1000.0f / avg : 0.0f, avg);
    if (!history.empty())
        ImGui::PlotLines("##frames", history.data(), static_cast<int>(history.size()), 0, nullptr, 0, 33.3f, {-1, 48});
    const auto& stats = engine_.renderer().stats();
    ImGui::Text("%s | %u sprites, %u draws, %u batches", std::string(engine_.gpu().driver()).c_str(), stats.sprites,
                stats.draw_calls, stats.batches);
    ImGui::Text("%zu entities, %zu archetypes | %u voices | %u workers", engine_.world().entity_count(),
                engine_.world().archetypes().size(), engine_.audio().active_voices(), engine_.jobs().worker_count());
    bool vsync = engine_.gpu().vsync();
    if (ImGui::Checkbox("vsync", &vsync)) engine_.gpu().set_vsync(vsync);

    // Time controls: pause, single-step and slow-motion make simulation bugs reproducible.
    ImGui::SeparatorText("Time");
    bool paused = engine_.paused();
    if (ImGui::Checkbox("Paused", &paused)) engine_.set_paused(paused);
    ImGui::SameLine();
    ImGui::BeginDisabled(!paused);
    if (ImGui::Button("Step tick")) engine_.step();
    ImGui::EndDisabled();
    f32 scale = engine_.time_scale();
    if (ImGui::SliderFloat("Time scale", &scale, 0.05f, 4.0f, "%.2fx", ImGuiSliderFlags_Logarithmic)) engine_.set_time_scale(scale);
    ImGui::Text("tick %llu  t=%.2fs  alpha %.2f", static_cast<unsigned long long>(engine_.tick()), engine_.time(), engine_.alpha());

    if (ImGui::CollapsingHeader("Profiler", ImGuiTreeNodeFlags_DefaultOpen)) draw_profiler();
    if (ImGui::CollapsingHeader("Post & lighting")) draw_post();
    if (ImGui::CollapsingHeader("Systems")) {
        draw_systems("fixed", engine_.fixed_schedule());
        draw_systems("frame", engine_.frame_schedule());
    }
    if (ImGui::CollapsingHeader("Entities")) draw_entities();
    if (ImGui::CollapsingHeader("Assets")) draw_assets();
    if (ImGui::CollapsingHeader("Log")) draw_log();
    ImGui::End();
}

void DevTools::draw_post() {
    gfx::PostSettings& p = engine_.renderer().post();
    ImGui::SliderFloat("exposure", &p.exposure, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
    ImGui::SliderFloat("contrast", &p.contrast, 0.5f, 1.5f);
    ImGui::SliderFloat("saturation", &p.saturation, 0.0f, 2.0f);
    ImGui::ColorEdit3("tint", &p.tint.r);
    ImGui::Checkbox("tonemap (ACES)", &p.tonemap);
    ImGui::SeparatorText("bloom");
    ImGui::Checkbox("bloom", &p.bloom);
    ImGui::BeginDisabled(!p.bloom);
    ImGui::SliderFloat("threshold", &p.bloom_threshold, 0.0f, 3.0f);
    ImGui::SliderFloat("knee", &p.bloom_knee, 0.0f, 1.0f);
    ImGui::SliderFloat("intensity", &p.bloom_intensity, 0.0f, 3.0f);
    ImGui::SliderFloat("radius", &p.bloom_radius, 0.2f, 3.0f);
    ImGui::EndDisabled();
    ImGui::SeparatorText("vignette");
    ImGui::SliderFloat("strength", &p.vignette, 0.0f, 1.0f);
    ImGui::SliderFloat("radius##vignette", &p.vignette_radius, 0.0f, 1.0f);
    ImGui::SliderFloat("softness", &p.vignette_softness, 0.01f, 1.0f);
    ImGui::SeparatorText("lighting");
    ImGui::Checkbox("2D lighting", &p.lighting);
    ImGui::ColorEdit3("ambient", &p.ambient.r, ImGuiColorEditFlags_Float);
    ImGui::Text("%u pipelines cached", engine_.renderer().stats().pipelines);
}

void DevTools::draw_profiler() {
    if (ImGui::BeginTable("zones", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("zone");
        ImGui::TableSetupColumn("ms");
        ImGui::TableSetupColumn("calls");
        ImGui::TableHeadersRow();
        for (const auto& z : profiler::last_frame()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(z.name.data(), z.name.data() + z.name.size());
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", z.milliseconds);
            ImGui::TableNextColumn();
            ImGui::Text("%u", z.calls);
        }
        ImGui::EndTable();
    }
}

void DevTools::draw_systems(const char* label, ecs::Schedule& schedule) {
    ImGui::PushID(label);
    bool parallel = schedule.parallel();
    ImGui::Text("%s schedule", label);
    ImGui::SameLine();
    if (ImGui::Checkbox("parallel", &parallel)) schedule.set_parallel(parallel);
    const auto& batches = schedule.batches();
    const auto& systems = schedule.systems();
    for (usize b = 0; b < batches.size(); ++b) {
        for (usize i : batches[b]) {
            auto& s = *systems[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Checkbox("##on", &s.enabled);
            ImGui::SameLine();
            ImGui::Text("[%zu] %-24s %.3f ms%s", b, s.name.c_str(), s.last_ms, s.exclusive ? "  (exclusive)" : "");
            ImGui::PopID();
        }
    }
    if (systems.empty()) ImGui::TextDisabled("  (no systems)");
    ImGui::PopID();
}


void DevTools::draw_entities() {
    auto& world = engine_.world();
    if (ImGui::Button("Save world")) {
        serial::Context ctx{&engine_.assets()};
        std::string text = json::stringify(ecs::save_world(world, ctx));
        bool ok = write_file_bytes("devtools_world.json", text.data(), text.size());
        info("dev tools: {} devtools_world.json ({} warnings)", ok ? "saved" : "could not write", ctx.warnings.size());
        for (const auto& w : ctx.warnings) warn("  {}", w);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load (adds to world)")) {
        if (auto text = read_file_text("devtools_world.json")) {
            serial::Context ctx{&engine_.assets()};
            auto parsed = json::parse(*text);
            usize n = parsed ? ecs::load_entities(world, parsed.value, ctx).size() : 0;
            info("dev tools: loaded {} entities ({} warnings)", n, ctx.warnings.size());
            for (const auto& w : ctx.warnings) warn("  {}", w);
        }
    }
    for (const auto& a : world.archetypes()) {
        if (a->empty()) continue;
        std::string name;
        for (ecs::ComponentId c : a->components()) {
            if (!name.empty()) name += ", ";
            name += ecs::component_info(c).name;
        }
        if (name.empty()) name = "(no components)";
        std::string label = std::format("{} x{}##{}", name, a->size(), a->id());
        if (ImGui::TreeNode(label.c_str())) {
            usize shown = std::min<usize>(a->size(), 64);
            for (usize i = 0; i < shown; ++i) {
                ecs::Entity e = a->entities()[i];
                std::string item = std::format("entity {} (gen {})", e.index, e.generation);
                if (ImGui::Selectable(item.c_str(), selected_ == e)) selected_ = e;
            }
            if (a->size() > shown) ImGui::TextDisabled("... %zu more", a->size() - shown);
            ImGui::TreePop();
        }
    }
}

void DevTools::draw_inspector() {
    auto& world = engine_.world();
    if (!selected_ || !world.alive(selected_)) {
        selected_ = {};
        return;
    }
    ImGui::SetNextWindowPos({450, 200}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({380, 420}, ImGuiCond_FirstUseEver);
    bool open = true;
    std::string title = std::format("Entity {}:{}###inspector", selected_.index, selected_.generation);
    if (!ImGui::Begin(title.c_str(), &open)) {
        ImGui::End();
        return;
    }
    ecs::Entity e = selected_;
    if (ImGui::Button("Duplicate")) {
        serial::Context ctx{&engine_.assets()};
        auto copy = ecs::load_entities(world, ecs::save_entities(world, std::span(&e, 1), ctx), ctx);
        if (!copy.empty()) selected_ = copy[0];
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy JSON")) {
        serial::Context ctx{&engine_.assets()};
        ImGui::SetClipboardText(json::stringify(ecs::save_entities(world, std::span(&e, 1), ctx)).c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Destroy")) {
        world.destroy(e);
        selected_ = {};
    }
    inspector::components(world, e, engine_.assets(), &selected_);
    ImGui::End();
    if (!open) selected_ = {};
}

void DevTools::draw_assets() {
    auto& assets = engine_.assets();
    bool hot = assets.hot_reload();
    if (ImGui::Checkbox("hot reload", &hot)) assets.set_hot_reload(hot);
    for (const auto& a : assets.list()) {
        ImGui::PushID(a.path.c_str());
        if (ImGui::SmallButton("reload")) assets.reload(a.path);
        ImGui::SameLine();
        if (a.loaded) ImGui::Text("%s  v%u  (%.*s)", a.path.c_str(), a.version, static_cast<int>(a.type.size()), a.type.data());
        else ImGui::TextColored({1, 0.4f, 0.4f, 1}, "%s  FAILED", a.path.c_str());
        ImGui::PopID();
    }
}

void DevTools::draw_log() {
    ImGui::BeginChild("log", {0, 220}, ImGuiChildFlags_Borders);
    for (const auto& line : logging::recent(300)) {
        ImVec4 color = line.level >= LogLevel::Error ? ImVec4{1, 0.4f, 0.4f, 1}
                       : line.level == LogLevel::Warn ? ImVec4{1, 0.8f, 0.3f, 1}
                                                      : ImVec4{0.85f, 0.85f, 0.85f, 1};
        ImGui::TextColored(color, "%8.2f  %s", line.time, line.text.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace ph

#else // !PH_DEV_TOOLS

namespace ph {
DevTools::DevTools(Engine& engine) : engine_(engine) {}
DevTools::~DevTools() = default;
void DevTools::handle_event(const SDL_Event&) {}
void DevTools::begin_frame() {}
void DevTools::end_frame(Game&) {}
} // namespace ph

#endif
