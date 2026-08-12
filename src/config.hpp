// win-fg — runtime config: enable gate, model, multiplier, and the synthesis
// tuning parameters. Sourced from environment variables and an optional
// conf.toml (a config file, when present, wins over the env defaults).
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>

namespace winfg {

struct Config {
    bool     enabled     = false;  // gate; loader also honours the manifest enable var
    int      model       = 4;      // 3 = symmetric flow, 4 = bidir + occlusion gate
    int      multiplier  = 2;      // generated presents per real present + 1
    float    flowScale   = 1.0f;   // scales solved flow magnitude
    // synthesis (wfg_synth) tuning
    float    beta        = 8.0f;   // softmax sharpness on importance Z
    float    lambda      = 0.6f;   // FB-consistency vs photometric weight
    float    epsilon     = 0.05f;  // disocclusion floor
    float    photoScale  = 6.0f;   // photometric residual scale into Z

    void sanitize() {
        if (model < 3) model = 3; if (model > 4) model = 4;
        if (multiplier < 2) multiplier = 2; if (multiplier > 4) multiplier = 4;
        if (flowScale < 0.05f) flowScale = 0.05f; if (flowScale > 4.0f) flowScale = 4.0f;
        if (beta < 0.0f) beta = 0.0f; if (lambda < 0.0f) lambda = 0.0f;
    }
};

static inline float envf(const char* k, float d) {
    const char* v = std::getenv(k); return v ? std::strtof(v, nullptr) : d;
}
static inline int envi(const char* k, int d) {
    const char* v = std::getenv(k); return v ? std::atoi(v) : d;
}

// key=value TOML-lite reader (flat keys, '#' comments) — enough for our knobs.
static inline void apply_toml(Config& c, const std::string& path) {
    std::ifstream f(path);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        auto h = line.find('#'); if (h != std::string::npos) line = line.substr(0, h);
        auto eq = line.find('='); if (eq == std::string::npos) continue;
        auto trim = [](std::string s){
            size_t a = s.find_first_not_of(" \t\r\n"); size_t b = s.find_last_not_of(" \t\r\n");
            return (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1); };
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        if (k.empty() || v.empty()) continue;
        if      (k == "enabled")    c.enabled = (v == "1" || v == "true");
        else if (k == "model")      c.model = std::atoi(v.c_str());
        else if (k == "multiplier") c.multiplier = std::atoi(v.c_str());
        else if (k == "flowScale")  c.flowScale = std::strtof(v.c_str(), nullptr);
        else if (k == "beta")       c.beta = std::strtof(v.c_str(), nullptr);
        else if (k == "lambda")     c.lambda = std::strtof(v.c_str(), nullptr);
        else if (k == "epsilon")    c.epsilon = std::strtof(v.c_str(), nullptr);
        else if (k == "photoScale") c.photoScale = std::strtof(v.c_str(), nullptr);
    }
}

// Path of the conf.toml the app writes (used for hot-reload mtime checks).
static inline std::string conf_path() {
    if (const char* p = std::getenv("WIN_FG_CONF")) return p;
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.config/win-fg/conf.toml" : std::string();
}

// env defaults first, then conf.toml overrides (file wins when present).
static inline Config load_config() {
    Config c;
    c.enabled    = envi("WIN_FG_ENABLE", 0) != 0;
    c.model      = envi("WIN_FG_MODEL", c.model);
    c.multiplier = envi("WIN_FG_MULT", c.multiplier);
    c.flowScale  = envf("WIN_FG_FLOWSCALE", c.flowScale);
    c.beta       = envf("WIN_FG_BETA", c.beta);
    c.lambda     = envf("WIN_FG_LAMBDA", c.lambda);
    c.epsilon    = envf("WIN_FG_EPSILON", c.epsilon);
    c.photoScale = envf("WIN_FG_PHOTOSCALE", c.photoScale);
    const char* home = std::getenv("HOME");
    if (home) apply_toml(c, std::string(home) + "/.config/win-fg/conf.toml");
    if (const char* p = std::getenv("WIN_FG_CONF")) apply_toml(c, p);
    c.sanitize();
    return c;
}

// UBO layout — MUST match wfg_synth.comp binding 0 (8 floats).
struct SynthUBO {
    float flowScale, alpha, beta, lambda, epsilon, photoScale, pad0, pad1;
};
// UBO layout — MUST match of3_flow / of3_expand_m4 binding 0 (Model3UBO).
struct FlowUBO { float flowScale; uint32_t level; float occlLo; float occlHi; };

} // namespace winfg
