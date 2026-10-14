#include "dev_tools.hpp"

#include <SDL3/SDL.h>

#if PH_DEV_TOOLS

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
    game.dev_ui(engine_);
    ImGui::Render();
}

void DevTools::draw_main() {
    ImGui::SetNextWindowPos({12, 12}, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize({420, 560}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("PLACEHOLDERENGINENAME")) {
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
    if (ImGui::CollapsingHeader("Systems")) {
        draw_systems("fixed", engine_.fixed_schedule());
        draw_systems("frame", engine_.frame_schedule());
    }
    if (ImGui::CollapsingHeader("Entities")) draw_entities();
    if (ImGui::CollapsingHeader("Assets")) draw_assets();
    if (ImGui::CollapsingHeader("Log")) draw_log();
    ImGui::End();
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
                ImGui::BulletText("entity %u (gen %u)", e.index, e.generation);
            }
            if (a->size() > shown) ImGui::TextDisabled("... %zu more", a->size() - shown);
            ImGui::TreePop();
        }
    }
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
