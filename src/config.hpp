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
    // C1 GLOBAL-MOTION PRE-WARP. Estimate the per-frame camera affine (LK) and
    // remove it before the dense SAD flow search so the search only sees coherent
    // object-only residual motion (kills fast-pan "melt"). 0 = auto (engage only
    // when the LK solve is stable), 1 = on (force-engage once a solve exists),
    // 2 = off (never pre-warp; identity affine → pipeline == pre-C1 exactly).
    // WIN_FG_GM=auto|on|off  /  conf.toml global_motion=auto|on|off.
    int      gmMode      = 0;      // 0 auto, 1 on, 2 off
    // C2 FLOW REGULARIZATION (TV-L1 smoothness prior). After C1 removes the camera
    // motion, the dense flow (flowLvl_ at kFlowFinest, 1/4-res) is the OBJECT-only
    // residual; it still carries spurious per-block vectors. C2 runs N semi-implicit
    // TV-L1 denoising iterations on that field — an L1 data term + edge-aware Total-
    // Variation regularizer — to kill incoherent vectors while KEEPING true motion
    // discontinuities (object edges) sharp. Solved in-place; expand's median + C1
    // add-back consume the cleaned field unchanged. 0 iterations ⇒ byte-identical to
    // pre-C2. See shaders/of3_flowreg.comp.
    // WIN_FG_FLOWREG=auto|on|off  /  conf.toml flow_reg=auto|on|off.
    int      frMode      = 0;      // 0 auto (engaged), 1 on (engaged), 2 off
    int      frIters     = 4;      // TV-L1 iterations at kFlowFinest (0 ⇒ off)
    float    frLambda    = 2.0f;   // L1 data-fidelity weight (higher ⇒ trust SAD flow)
    float    frDt        = 0.25f;  // semi-implicit smoothing step (higher ⇒ smoother)
    float    frEdge      = 8.0f;   // luma-gradient edge sensitivity for the TV weight g
    float    frEps       = 0.05f;  // Charbonnier epsilon (px) for the TV diffusivity
    // synthesis (wfg_synth) tuning
    float    beta        = 8.0f;   // softmax sharpness on importance Z
    float    lambda      = 0.6f;   // FB-consistency vs photometric weight
    // Tightened 2026-08-17 to prioritise "no ghost" over max sharpness — the
    // remaining single-frame ghosts came from pixels that the previous defaults
    // still trusted with warp when they should have cross-faded. Raising both
    // pushes borderline pixels toward the safe cross-fade path (softer, but no
    // smear). Prewarp will lift the ceiling further; this is the "safety net"
    // knob change until then.
    // Walked back from (0.10, 9.0) tighten after user reported softening. The
    // real fix for the residual ghosts is the 3a-fallback path change in
    // layer.cpp (present real curr on fallback instead of blitting synth over);
    // the gate can stay closer to the original values now.
    float    epsilon     = 0.07f;  // disocclusion floor (0.05 -> 0.10 -> 0.07)
    float    photoScale  = 7.5f;   // photometric residual scale into Z (6.0 -> 9.0 -> 7.5)
    // HUD exclusion rect in pixel coords (x0,y0,x1,y1). Fragments inside are
    // passed through as the real current frame (no warp, no synth) so text /
    // overlays don't ghost. Disabled when x0 >= x1 or y0 >= y1 (the default).
    // Sourced from WIN_FG_HUD_RECT="x0,y0,x1,y1" env var or conf.toml
    // (hudRect="x0,y0,x1,y1"). Pattern is Isygold's Vegas DXVK framegen —
    // host knows where the HUD is, tell the shader to skip it.
    float    hudX0       = 0.0f;
    float    hudY0       = 0.0f;
    float    hudX1       = 0.0f;
    float    hudY1       = 0.0f;

    void sanitize() {
        if (model < 3) model = 3; if (model > 4) model = 4;
        if (multiplier < 2) multiplier = 2; if (multiplier > 4) multiplier = 4;
        if (gmMode < 0) gmMode = 0; if (gmMode > 2) gmMode = 2;
        if (frMode < 0) frMode = 0; if (frMode > 2) frMode = 2;
        if (frIters < 0) frIters = 0; if (frIters > 16) frIters = 16;
        if (frLambda < 0.0f) frLambda = 0.0f;
        if (frDt < 0.01f) frDt = 0.01f; if (frDt > 4.0f) frDt = 4.0f;
        if (frEdge < 0.0f) frEdge = 0.0f;
        if (frEps < 1e-3f) frEps = 1e-3f;
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

// "auto"/"on"/"off" (also true/false/on/off numerics) -> {0 auto, 1 on, 2 off}.
static inline int parse_tristate(const std::string& v, int dflt) {
    if (v.empty()) return dflt;
    if (v == "auto") return 0;
    if (v == "on"  || v == "1" || v == "force" || v == "true"  || v == "yes") return 1;
    if (v == "off" || v == "0" || v == "false" || v == "no")                  return 2;
    return dflt;
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
        else if (k == "global_motion") c.gmMode = parse_tristate(v, c.gmMode);
        else if (k == "flow_reg")   c.frMode = parse_tristate(v, c.frMode);
        else if (k == "fr_iters")   c.frIters = std::atoi(v.c_str());
        else if (k == "fr_lambda")  c.frLambda = std::strtof(v.c_str(), nullptr);
        else if (k == "fr_dt")      c.frDt = std::strtof(v.c_str(), nullptr);
        else if (k == "fr_edge")    c.frEdge = std::strtof(v.c_str(), nullptr);
        else if (k == "fr_eps")     c.frEps = std::strtof(v.c_str(), nullptr);
        else if (k == "flowScale")  c.flowScale = std::strtof(v.c_str(), nullptr);
        else if (k == "beta")       c.beta = std::strtof(v.c_str(), nullptr);
        else if (k == "lambda")     c.lambda = std::strtof(v.c_str(), nullptr);
        else if (k == "epsilon")    c.epsilon = std::strtof(v.c_str(), nullptr);
        else if (k == "photoScale") c.photoScale = std::strtof(v.c_str(), nullptr);
        else if (k == "hudRect") {
            // "x0,y0,x1,y1" in pixel coords
            float r[4] = {0,0,0,0}; int n = 0;
            size_t start = 0; std::string s = v;
            for (; n < 4; ++n) {
                size_t comma = s.find(',', start);
                std::string tok = s.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
                r[n] = std::strtof(tok.c_str(), nullptr);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
            c.hudX0 = r[0]; c.hudY0 = r[1]; c.hudX1 = r[2]; c.hudY1 = r[3];
        }
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
    if (const char* g = std::getenv("WIN_FG_GM")) c.gmMode = parse_tristate(g, c.gmMode);
    if (const char* r = std::getenv("WIN_FG_FLOWREG")) c.frMode = parse_tristate(r, c.frMode);
    c.frIters    = envi("WIN_FG_FR_ITERS", c.frIters);
    c.frLambda   = envf("WIN_FG_FR_LAMBDA", c.frLambda);
    c.frDt       = envf("WIN_FG_FR_DT", c.frDt);
    c.frEdge     = envf("WIN_FG_FR_EDGE", c.frEdge);
    c.frEps      = envf("WIN_FG_FR_EPS", c.frEps);
    c.flowScale  = envf("WIN_FG_FLOWSCALE", c.flowScale);
    c.beta       = envf("WIN_FG_BETA", c.beta);
    c.lambda     = envf("WIN_FG_LAMBDA", c.lambda);
    c.epsilon    = envf("WIN_FG_EPSILON", c.epsilon);
    c.photoScale = envf("WIN_FG_PHOTOSCALE", c.photoScale);
    if (const char* h = std::getenv("WIN_FG_HUD_RECT")) {
        // "x0,y0,x1,y1" — pixel coords; disabled when x0>=x1 or y0>=y1
        float r[4] = {0,0,0,0}; int n = 0;
        std::string s = h; size_t start = 0;
        for (; n < 4; ++n) {
            size_t comma = s.find(',', start);
            r[n] = std::strtof(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start).c_str(), nullptr);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        c.hudX0 = r[0]; c.hudY0 = r[1]; c.hudX1 = r[2]; c.hudY1 = r[3];
    }
    const char* home = std::getenv("HOME");
    if (home) apply_toml(c, std::string(home) + "/.config/win-fg/conf.toml");
    if (const char* p = std::getenv("WIN_FG_CONF")) apply_toml(c, p);
    c.sanitize();
    return c;
}

// UBO layout — MUST match wfg_synth.comp binding 0.
// The vec4 hudRect is 16-byte aligned in std140, sits at offset 32 (after the
// 8 leading floats which naturally occupy 32B). Disabled when hudX0>=hudX1.
struct SynthUBO {
    float flowScale, alpha, beta, lambda, epsilon, photoScale, pad0, pad1;
    float hudX0, hudY0, hudX1, hudY1;
};
// UBO layout — MUST match of3_flow / of3_expand_m4 binding 0 (Model3UBO).
struct FlowUBO { float flowScale; uint32_t level; float occlLo; float occlHi; };

// C1 global-motion UBO — MUST match binding 1 in of3_gm_reduce / of3_gm_prewarp /
// of3_expand(_m4). std140: four vec4 = 64 bytes. The affine is in centered-UV
// space: P = [[p1,p3],[p2,p4]], translation (tx,ty); warp uvSrc = uv + P*(uv-0.5)
// + t maps a prev(template) coord to the curr(image) coord it samples (prev→curr).
//   lin[]  = running LK estimate (linearization point fed to of3_gm_reduce)
//   app[]  = APPLIED affine (of3_gm_prewarp + expand add-back); identity when
//            disengaged so the pipeline is byte-identical to pre-C1.
struct GMUBO {
    float lin[4];   // p1,p2,p3,p4
    float linT[4];  // tx,ty, engaged, pad
    float app[4];   // p1,p2,p3,p4
    float appT[4];  // tx,ty, pad, pad
};

// C2 flow-regularization UBO — MUST match binding 0 (vec4 params) in
// of3_flowreg.comp. std140: one vec4 = 16 bytes. See Config::fr* fields.
struct FlowRegUBO {
    float dt;         // params.x — semi-implicit smoothing step
    float lambda;     // params.y — L1 data-fidelity weight (θ = dt·lambda)
    float edgeAlpha;  // params.z — luma-gradient edge sensitivity for g
    float epsTV;      // params.w — Charbonnier epsilon (px) for the TV diffusivity
};

} // namespace winfg
