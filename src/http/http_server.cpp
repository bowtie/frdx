#include "fh6/http/http_server.hpp"
#include "fh6/audio_source_manager.hpp"
#include "fh6/config_store.hpp"
#include "fh6/fmod/dsp_bridge.hpp"
#include "fh6/log.hpp"
#include "fh6/sources/shoutcast_source.hpp"

#define CPPHTTPLIB_NO_EXCEPTIONS
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace fh6::http {

using json = nlohmann::json;

namespace {

constexpr const char* state_string(PlaybackState s) noexcept {
    switch (s) {
        case PlaybackState::stopped: return "stopped";
        case PlaybackState::playing: return "playing";
        case PlaybackState::paused: return "paused";
        case PlaybackState::buffering: return "buffering";
    }
    return "unknown";
}
constexpr const char* auth_string(AuthState s) noexcept {
    switch (s) {
        case AuthState::none_required: return "none_required";
        case AuthState::authenticated: return "authenticated";
        case AuthState::needs_auth: return "needs_auth";
        case AuthState::error: return "error";
    }
    return "unknown";
}
constexpr const char* mode_string(fmod_bridge::DSPMode m) noexcept {
    switch (m) {
        case fmod_bridge::DSPMode::off: return "off";
        case fmod_bridge::DSPMode::passthrough: return "passthrough";
        case fmod_bridge::DSPMode::silence: return "silence";
        case fmod_bridge::DSPMode::pcm: return "pcm";
    }
    return "unknown";
}

json track_to_json(const TrackInfo& t) {
    return json{
        {"title", t.title},
        {"artist", t.artist},
        {"album", t.album},
        {"artwork_url", t.artwork_url},
        {"duration_ms", t.duration_ms},
        {"position_ms", t.position_ms},
    };
}
json source_to_json(IAudioSource* s) {
    auto c = s->capabilities();
    return json{
        {"name", std::string{s->name()}},
        {"display_name", std::string{s->display_name()}},
        {"playback_state", state_string(s->playback_state())},
        {"auth_state", auth_string(s->auth_state())},
        {"auth_instructions", s->auth_instructions()},
        {"capabilities",
         json{
             {"seek", c.seek},
             {"previous", c.previous},
             {"queue", c.queue},
         }},
        {"details", json::object()},
    };
}

std::string path_s(const std::filesystem::path& p) {
    return p.empty() ? std::string{} : p.string();
}

json config_to_json(const Config& c) {
    return json{
        {"general",
         json{
             {"port", c.general.port},
             {"ring_buffer_mb", c.general.ring_buffer_mb},
             {"default_source", c.general.default_source},
             {"fallback_source", c.general.fallback_source},
         }},
        {"shoutcast",
         json{
             {"enabled", c.shoutcast.enabled},
             {"default_url", c.shoutcast.default_url},
             {"ffmpeg_path", path_s(c.shoutcast.ffmpeg_path)},
             {"presets", c.shoutcast.presets},
         }},
        {"audio",
         json{
             {"output_gain", c.audio.output_gain},
         }},
    };
}

template <class T> T pull(const json& tbl, const char* k, T fallback) {
    if (auto it = tbl.find(k); it != tbl.end() && !it->is_null()) {
        try {
            return it->get<T>();
        } catch (...) {}
    }
    return fallback;
}
std::filesystem::path pull_path(const json& tbl, const char* k,
                                const std::filesystem::path& fallback) {
    auto s = pull<std::string>(tbl, k, path_s(fallback));
    return s.empty() ? std::filesystem::path{} : std::filesystem::path{s};
}

// Deep-merge a partial JSON patch into Config. Absent keys keep their value.
void apply_patch(Config& c, const json& j) {
    if (auto it = j.find("general"); it != j.end()) {
        c.general.port            = pull(*it, "port", c.general.port);
        c.general.ring_buffer_mb  = pull(*it, "ring_buffer_mb", c.general.ring_buffer_mb);
        c.general.default_source  = pull(*it, "default_source", c.general.default_source);
        c.general.fallback_source = pull(*it, "fallback_source", c.general.fallback_source);
    }
    if (auto it = j.find("shoutcast"); it != j.end()) {
        c.shoutcast.enabled      = pull(*it, "enabled", c.shoutcast.enabled);
        c.shoutcast.default_url  = pull(*it, "default_url", c.shoutcast.default_url);
        c.shoutcast.ffmpeg_path  = pull_path(*it, "ffmpeg_path", c.shoutcast.ffmpeg_path);
        if (auto p = it->find("presets"); p != it->end() && p->is_array())
            c.shoutcast.presets = p->get<std::vector<std::string>>();
    }
    if (auto it = j.find("audio"); it != j.end()) {
        c.audio.output_gain = pull(*it, "output_gain", c.audio.output_gain);
    }
}


void cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type");
}
void ok(httplib::Response& res, const json& body = json::object()) {
    res.set_content(body.empty() ? std::string{R"({"ok":true})"} : body.dump(), "application/json");
}
void fail(httplib::Response& res, int status, std::string_view msg) {
    res.status = status;
    res.set_content(json{{"error", std::string{msg}}}.dump(), "application/json");
}

} // namespace

struct HttpServer::Impl {
    AudioSourceManager& mgr;
    fmod_bridge::DSPBridge& bridge;
    ConfigStore& store;
    std::filesystem::path ui_dist;
    httplib::Server svr;
    std::jthread thr;
    std::atomic<bool> stopping{false};

    Impl(AudioSourceManager& m, fmod_bridge::DSPBridge& b, ConfigStore& s, uint16_t port,
         std::filesystem::path dist)
        : mgr{m}, bridge{b}, store{s}, ui_dist{std::move(dist)} {
        wire_routes();
        thr = std::jthread([this, port](const std::stop_token&) {
            log::info("[http] listening on http://localhost:{}", port);
            svr.listen("0.0.0.0", port);
        });
    }
    ~Impl() {
        stopping.store(true);
        svr.stop();
    }

    json build_state() const {
        auto* a      = mgr.active();
        json sources = json::array();
        for (auto* s : mgr.sources_snapshot()) sources.push_back(source_to_json(s));
        return json{
            {"game", json{{"attached", true}, {"injector_ready", true}}},
            {"audio",
             json{
                 {"active", bridge.mode() == fmod_bridge::DSPMode::pcm},
                 {"native_dsp_mode", mode_string(bridge.mode())},
                 {"output_gain", bridge.gain()},
                 {"underruns", bridge.underruns()},
                 {"calls", bridge.call_count()},
                 {"buffer_len", bridge.last_buffer_len()},
                 {"out_channels", bridge.last_out_channels()},
                 {"ring_avail", mgr.ring().readable()},
                 {"ring_capacity", mgr.ring().capacity()},
             }},
            {"sources",
             {
                 {"active", a ? std::string{a->name()} : ""},
                 {"available", std::move(sources)},
             }},
            {"track", a ? track_to_json(a->current_track()) : json::object()},
            {"errors", json::array()},
        };
    }

    IAudioSource* find(std::string_view name) const {
        for (auto* s : mgr.sources_snapshot())
            if (s->name() == name) return s;
        return nullptr;
    }
    template <class T> T* find_typed(std::string_view name) const {
        return dynamic_cast<T*>(find(name));
    }

    void wire_routes() {
        svr.Options(R"(/.*)", [](const httplib::Request&, httplib::Response& r) { cors(r); });

        // Live state
        svr.Get("/api/state", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, build_state());
        });
        svr.Get("/api/events", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            r.set_header("Cache-Control", "no-store");
            r.set_chunked_content_provider(
                "text/event-stream", [this](std::size_t, httplib::DataSink& sink) {
                    std::string ev = "data: " + build_state().dump() + "\n\n";
                    if (!sink.write(ev.data(), ev.size())) return false;
                    std::this_thread::sleep_for(std::chrono::milliseconds(750));
                    return !stopping.load(std::memory_order_acquire);
                });
        });

        // Source switching + transport
        svr.Get("/api/sources", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, build_state()["sources"]);
        });
        svr.Post("/api/source/switch", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto src = json::parse(q.body).at("source").get<std::string>();
                if (mgr.switch_to(src)) {
                    ok(r);
                } else {
                    fail(r, 404, "unknown source");
                }
            } catch (...) {
                fail(r, 400, "bad body");
            }
        });
        svr.Post(R"(/api/source/([^/]+)/(play|pause|stop|next|previous))",
                 [this](const httplib::Request& q, httplib::Response& r) {
                     cors(r);
                     auto* s = find(q.matches[1].str());
                     if (!s) {
                         fail(r, 404, "unknown source");
                         return;
                     }
                     const auto act       = q.matches[2].str();
                     const bool is_active = (s == mgr.active());
                     if (act == "play") {
                         s->play();
                     } else if (act == "pause") {
                         s->pause();
                     } else if (act == "stop") {
                         s->stop();
                         if (is_active) mgr.ring().drain();
                     } else if (act == "next") {
                         s->next();
                         if (is_active) mgr.ring().drain();
                     } else if (act == "previous") {
                         s->previous();
                         if (is_active) mgr.ring().drain();
                     }
                     ok(r);
                 });

        // Shoutcast-specific runtime actions
        svr.Post("/api/source/shoutcast/cast",
                 [this](const httplib::Request& q, httplib::Response& r) {
                     cors(r);
                     auto* sc = find_typed<sources::ShoutcastSource>("shoutcast");
                     if (!sc) {
                         fail(r, 404, "shoutcast not registered");
                         return;
                     }
                     try {
                         auto url = json::parse(q.body).at("url").get<std::string>();
                         const bool was_active = (mgr.active() == sc);
                         sc->set_target(std::move(url));
                         sc->stop();
                         if (was_active) mgr.ring().drain();
                         sc->play();
                         mgr.switch_to("shoutcast");
                         ok(r);
                     } catch (...) {
                         fail(r, 400, "bad body");
                     }
                 });
        svr.Get("/api/source/shoutcast/presets",
                [this](const httplib::Request&, httplib::Response& r) {
                    cors(r);
                    auto* sc = find_typed<sources::ShoutcastSource>("shoutcast");
                    if (!sc) {
                        fail(r, 404, "shoutcast not registered");
                        return;
                    }
                    ok(r, json{{"presets", store.snapshot().shoutcast.presets}});
                });

        // Station preset catalog (stations.json)
        svr.Get("/api/source/shoutcast/stations",
                [this](const httplib::Request&, httplib::Response& r) {
                    cors(r);
                    auto* sc = find_typed<sources::ShoutcastSource>("shoutcast");
                    if (!sc) {
                        fail(r, 404, "shoutcast not registered");
                        return;
                    }
                    auto stations_json = json::array();
                    for (const auto& sp : sc->stations()) {
                        auto streams_json = json::array();
                        for (const auto& ss : sp.streams) {
                            streams_json.push_back(json{
                                {"id", ss.id},
                                {"name", ss.name},
                                {"url", ss.url},
                                {"icon", ss.icon},
                            });
                        }
                        auto sp_json = json{
                            {"id", sp.id},
                            {"name", sp.name},
                            {"description", sp.description},
                            {"icon", sp.icon},
                            {"streams", std::move(streams_json)},
                        };
                        if (sp.metadata.type == "rest") {
                            sp_json["metadata"] = json{
                                {"type", "rest"},
                                {"poll_interval_secs", sp.metadata.poll_interval_secs},
                            };
                        }
                        stations_json.push_back(std::move(sp_json));
                    }
                    ok(r, json{{"stations", std::move(stations_json)}});
                });

        svr.Post("/api/source/shoutcast/station/select",
                 [this](const httplib::Request& q, httplib::Response& r) {
                     cors(r);
                     auto* sc = find_typed<sources::ShoutcastSource>("shoutcast");
                     if (!sc) {
                         fail(r, 404, "shoutcast not registered");
                         return;
                     }
                     try {
                         auto body      = json::parse(q.body);
                         auto station   = body.at("station_id").get<std::string>();
                         auto stream_id = body.value("stream_id", "");
                         const bool was_active = (mgr.active() == sc);
                         sc->select_station(station, stream_id);
                         if (was_active) mgr.ring().drain();
                         mgr.switch_to("shoutcast");
                         ok(r);
                     } catch (...) {
                         fail(r, 400, "bad body");
                     }
                 });

        // Config editing
        svr.Get("/api/config", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            ok(r, config_to_json(store.snapshot()));
        });
        svr.Put("/api/config", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto patch = json::parse(q.body);
                store.patch([&](Config& c) { apply_patch(c, patch); });
                ok(r, config_to_json(store.snapshot()));
            } catch (const std::exception& e) {
                fail(r, 400, e.what());
            }
        });
        svr.Post("/api/config/reload", [this](const httplib::Request&, httplib::Response& r) {
            cors(r);
            store.reload();
            ok(r, config_to_json(store.snapshot()));
        });

        // Fast-path audio knobs (currently just output_gain, the slider).
        svr.Post("/api/options", [this](const httplib::Request& q, httplib::Response& r) {
            cors(r);
            try {
                auto j = json::parse(q.body);
                if (j.contains("output_gain")) {
                    float g = std::clamp(j.at("output_gain").get<float>(), 0.0f, 1.0f);
                    bridge.set_gain(g);
                    store.patch([&](Config& c) { c.audio.output_gain = g; });
                }
                ok(r);
            } catch (...) {
                fail(r, 400, "bad body");
            }
        });

        if (!ui_dist.empty() && std::filesystem::exists(ui_dist))
            svr.set_mount_point("/", ui_dist.string());
    }
};

HttpServer::HttpServer(AudioSourceManager& mgr, fmod_bridge::DSPBridge& bridge, ConfigStore& cfg,
                       uint16_t port, std::filesystem::path ui_dist)
    : impl_{std::make_unique<Impl>(mgr, bridge, cfg, port, std::move(ui_dist))} {}

HttpServer::~HttpServer() = default;

} // namespace fh6::http
