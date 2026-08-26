// win-fg — TRAINING-DATA CAPTURE ENGINE (CPU side).
//
// A dev-only sink for the real, pre-interpolation game frames the layer sees at
// present time, for building a Route-B VFI (video frame interpolation) dataset
// offline. layer.cpp does the GPU-side downscale + async readback and hands this
// engine tightly-packed RGBA8 byte buffers (one per real present, in order);
// everything here is plain CPU work on a dedicated background thread so the
// present thread only pays for a memcpy + enqueue.
//
// NO Vulkan in this file — it never touches the dispatch table, so it can't race
// the render path. NO third-party code: the QOI encoder below is a clean-room
// implementation of the public-domain QOI spec (qoiformat.org); the container,
// motion and patch logic are our own.
//
// ── OUTPUT: a small number of large PACKED containers, not thousands of files ──
//   <root>/session_<epoch_ms>/
//     shard_000.wfgcap , shard_001.wfgcap , ...   (rolls at capShardMB)
//     manifest.jsonl                              (header line + one line/record)
//
// Everything is LOSSLESS (raw RGBA8 → QOI, a lossless codec). NEVER a lossy video
// codec — compression artifacts would ruin the training data.
//
// ── .wfgcap CONTAINER BYTE LAYOUT (all little-endian) ─────────────────────────
//   File header (40 bytes):
//     char magic[8] = "WFGCAP01"
//     u32 version    (=1)
//     u32 mode       (0 = patch/triplet, 1 = frame)
//     u32 patch      (patch edge in downscaled px; 0 in frame mode)
//     u32 dstW, dstH (downscaled frame dims)
//     u32 srcW, srcH (original swapchain dims)
//     u32 reserved   (=0)
//   Then a stream of self-describing records:
//     Record header (32 bytes):
//       u8  type     (0 patch, 1 frame)
//       u8  nblobs   (patch: #coords; frame: 1)
//       u16 pad0
//       u32 seq      (unit sequence, monotonic within the session)
//       u64 src      (source present index; patch mode = CENTER frame's index)
//       u64 ts_ns    (steady-clock ns at capture of the center/frame)
//       f32 motion   (mean per-pixel luma abs-diff, 0..255)
//       u32 pad1
//     Then nblobs blobs:
//       u16 x, u16 y (patch top-left in downscaled space; 0,0 in frame mode)
//       u16 w, u16 h (blob image dims: patch = 3*patch × patch, frame = dstW × dstH)
//       u32 qoi_len
//       u8  qoi[qoi_len]   ← LOSSLESS QOI. Patch blob is [prev | center | next]
//                            packed left-to-right; center is the interp TARGET.
//   The sidecar manifest.jsonl additionally records each record's shard, byte
//   offset, and each blob's absolute file offset+len for random access.
#pragma once
#include "log.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <utility>
#include <sys/stat.h>
#include <sys/types.h>

namespace winfg {

// ── QOI encoder (clean-room; public-domain QOI spec) ─────────────────────────
// Encodes tightly-packed RGBA8 (channels=4) losslessly. See qoiformat.org.
static inline void qoi_encode(const uint8_t* rgba, int w, int h, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve((size_t)w * h * 4 / 2 + 64);
    auto put = [&](uint8_t b){ out.push_back(b); };
    auto put32 = [&](uint32_t v){ put((v>>24)&0xff); put((v>>16)&0xff); put((v>>8)&0xff); put(v&0xff); };
    put('q'); put('o'); put('i'); put('f');
    put32((uint32_t)w); put32((uint32_t)h);
    put(4);   // channels
    put(0);   // colorspace (sRGB w/ linear alpha) — metadata only, lossless regardless
    uint8_t idxR[64]={0}, idxG[64]={0}, idxB[64]={0}, idxA[64]={0};
    uint8_t pr=0, pg=0, pb=0, pa=255;   // previous pixel
    int run = 0;
    const size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; ++i) {
        uint8_t r = rgba[i*4+0], g = rgba[i*4+1], b = rgba[i*4+2], a = rgba[i*4+3];
        if (r==pr && g==pg && b==pb && a==pa) {
            ++run;
            if (run == 62 || i == n-1) { put(0xC0 | (uint8_t)(run-1)); run = 0; }
        } else {
            if (run > 0) { put(0xC0 | (uint8_t)(run-1)); run = 0; }
            int hash = (r*3 + g*5 + b*7 + a*11) & 63;
            if (idxR[hash]==r && idxG[hash]==g && idxB[hash]==b && idxA[hash]==a) {
                put(0x00 | (uint8_t)hash);                       // INDEX
            } else {
                idxR[hash]=r; idxG[hash]=g; idxB[hash]=b; idxA[hash]=a;
                if (a == pa) {
                    int8_t vr = (int8_t)(uint8_t)(r - pr);
                    int8_t vg = (int8_t)(uint8_t)(g - pg);
                    int8_t vb = (int8_t)(uint8_t)(b - pb);
                    int8_t vgr = (int8_t)(vr - vg);
                    int8_t vgb = (int8_t)(vb - vg);
                    if (vr>=-2 && vr<=1 && vg>=-2 && vg<=1 && vb>=-2 && vb<=1) {
                        put(0x40 | (uint8_t)((vr+2)<<4) | (uint8_t)((vg+2)<<2) | (uint8_t)(vb+2)); // DIFF
                    } else if (vgr>=-8 && vgr<=7 && vg>=-32 && vg<=31 && vgb>=-8 && vgb<=7) {
                        put(0x80 | (uint8_t)(vg+32));                                              // LUMA
                        put((uint8_t)((vgr+8)<<4) | (uint8_t)(vgb+8));
                    } else {
                        put(0xFE); put(r); put(g); put(b);                                         // RGB
                    }
                } else {
                    put(0xFF); put(r); put(g); put(b); put(a);                                     // RGBA
                }
            }
        }
        pr=r; pg=g; pb=b; pa=a;
    }
    for (int k = 0; k < 7; ++k) put(0);
    put(1);
}

struct CaptureConfig {
    int         mode        = 0;      // 0 patch, 1 frame
    int         patchSize   = 256;
    int         nPatches    = 3;
    float       motionThresh= 2.0f;   // mean per-pixel luma abs-diff (0..255)
    std::string root;                 // capture root (session subdir made under it)
    int         srcW = 0, srcH = 0;   // original swapchain res
    int         dstW = 0, dstH = 0;   // downscaled res (buffer dims)
    uint64_t    shardCapBytes = 1024ull * 1024 * 1024;
};

// One downscaled real frame handed over from the present thread.
struct CapFrame {
    std::vector<uint8_t> rgba;   // tightly packed dstW*dstH*4
    int      w = 0, h = 0;
    uint64_t src = 0;            // source present index (monotonic, +1 per real present)
    uint64_t ts  = 0;            // steady-clock ns at capture
};

class CaptureEngine {
public:
    bool running() const { return running_.load(); }
    uint64_t written()   const { return written_.load(); }
    uint64_t patches()   const { return patchesWritten_.load(); }
    uint64_t skipped()   const { return skipped_.load(); }
    uint64_t gaps()      const { return gaps_.load(); }
    uint64_t drops()     const { return drops_.load(); }
    uint64_t bytes()     const { return bytesWritten_.load(); }
    const std::string& sessionDir() const { return sessionDir_; }

    // Open the session dir + first shard + manifest and spawn the encoder thread.
    // Returns false (and stays not-running) if output can't be created.
    bool start(const CaptureConfig& cfg) {
        if (running_.load()) return true;
        cfg_ = cfg;
        mkdir_p(cfg_.root);
        uint64_t ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()).count();
        char sub[64]; std::snprintf(sub, sizeof(sub), "/session_%llu", (unsigned long long)ms);
        sessionDir_ = cfg_.root + sub;
        if (mkdir(sessionDir_.c_str(), 0777) != 0 && !dir_exists(sessionDir_)) {
            WFG_LOGE("capture: cannot create session dir %s", sessionDir_.c_str());
            return false;
        }
        std::string mp = sessionDir_ + "/manifest.jsonl";
        manifest_ = std::fopen(mp.c_str(), "wb");
        if (!manifest_) { WFG_LOGE("capture: cannot open manifest %s", mp.c_str()); return false; }
        if (!open_shard(0)) { std::fclose(manifest_); manifest_ = nullptr; return false; }
        std::fprintf(manifest_,
            "{\"win_fg_capture\":1,\"container\":\"wfgcap01\",\"format\":\"qoi-rgba8\",\"mode\":\"%s\","
            "\"src_w\":%d,\"src_h\":%d,\"dst_w\":%d,\"dst_h\":%d,\"patch\":%d,\"patches_per_triplet\":%d,"
            "\"motion_thresh\":%.3f,\"shard_cap_bytes\":%llu,"
            "\"triplet\":\"patch blob = [prev|center|next] left-to-right; center is the interpolation target\"}\n",
            cfg_.mode == 0 ? "patch" : "frame", cfg_.srcW, cfg_.srcH, cfg_.dstW, cfg_.dstH,
            cfg_.patchSize, cfg_.nPatches, cfg_.motionThresh, (unsigned long long)cfg_.shardCapBytes);
        std::fflush(manifest_);
        stop_.store(false);
        running_.store(true);
        rng_ = 0x9e3779b97f4a7c15ull ^ ms;
        thread_ = std::thread([this]{ this->loop(); });
        return true;
    }

    // Present-thread hand-off: copy the readback bytes and enqueue. Bounded queue —
    // if the encoder falls behind we DROP the oldest (logged as a gap) rather than
    // stall the game or grow unbounded.
    void submit(const uint8_t* rgba, int w, int h, uint64_t src, uint64_t ts) {
        if (!running_.load()) return;
        CapFrame f; f.w = w; f.h = h; f.src = src; f.ts = ts;
        f.rgba.assign(rgba, rgba + (size_t)w * h * 4);
        {
            std::lock_guard<std::mutex> lk(qlock_);
            if (queue_.size() >= kMaxQueue) { queue_.pop_front(); drops_.fetch_add(1); }
            queue_.push_back(std::move(f));
        }
        qcv_.notify_one();
    }

    // Flush the queue, join the thread, close shard + manifest. Idempotent.
    void stop() {
        if (!running_.exchange(false)) { if (thread_.joinable()) thread_.join(); return; }
        stop_.store(true);
        qcv_.notify_all();
        if (thread_.joinable()) thread_.join();
        if (shard_)    { std::fflush(shard_);    std::fclose(shard_);    shard_ = nullptr; }
        if (manifest_) { std::fflush(manifest_); std::fclose(manifest_); manifest_ = nullptr; }
    }

    ~CaptureEngine() { stop(); }

private:
    static const size_t kMaxQueue = 8;   // ~8 * dst frame bytes memory cap

    // ---- little-endian byte writers into a vector ----
    static void wu8 (std::vector<uint8_t>& v, uint8_t x)  { v.push_back(x); }
    static void wu16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x&0xff); v.push_back((x>>8)&0xff); }
    static void wu32(std::vector<uint8_t>& v, uint32_t x) { for (int i=0;i<4;++i) v.push_back((x>>(8*i))&0xff); }
    static void wu64(std::vector<uint8_t>& v, uint64_t x) { for (int i=0;i<8;++i) v.push_back((x>>(8*i))&0xff); }
    static void wf32(std::vector<uint8_t>& v, float f)    { uint32_t u; std::memcpy(&u,&f,4); wu32(v,u); }

    // ---- filesystem helpers ----
    static bool dir_exists(const std::string& p) { struct stat s; return stat(p.c_str(), &s) == 0 && (s.st_mode & S_IFDIR); }
    static void mkdir_p(const std::string& path) {
        std::string cur;
        for (size_t i = 0; i < path.size(); ++i) {
            cur += path[i];
            if (path[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0777);
        }
        mkdir(path.c_str(), 0777);
    }

    void shard_name(int idx, char* buf, size_t n) const { std::snprintf(buf, n, "shard_%03d.wfgcap", idx); }

    bool open_shard(int idx) {
        if (shard_) { std::fflush(shard_); std::fclose(shard_); shard_ = nullptr; }
        shardIdx_ = idx;
        char nm[64]; shard_name(idx, nm, sizeof(nm));
        curShard_ = nm;
        std::string path = sessionDir_ + "/" + curShard_;
        shard_ = std::fopen(path.c_str(), "wb");
        if (!shard_) { WFG_LOGE("capture: cannot open shard %s", path.c_str()); return false; }
        std::vector<uint8_t> hdr;
        const char magic[8] = {'W','F','G','C','A','P','0','1'};
        for (int i = 0; i < 8; ++i) wu8(hdr, (uint8_t)magic[i]);
        wu32(hdr, 1);                       // version
        wu32(hdr, (uint32_t)cfg_.mode);
        wu32(hdr, (uint32_t)(cfg_.mode == 0 ? cfg_.patchSize : 0));
        wu32(hdr, (uint32_t)cfg_.dstW); wu32(hdr, (uint32_t)cfg_.dstH);
        wu32(hdr, (uint32_t)cfg_.srcW); wu32(hdr, (uint32_t)cfg_.srcH);
        wu32(hdr, 0);                       // reserved
        std::fwrite(hdr.data(), 1, hdr.size(), shard_);
        shardBytes_ = hdr.size();
        bytesWritten_.fetch_add(hdr.size());
        return true;
    }

    // Roll to a fresh shard if the current one is at/over the cap, then append the
    // record bytes. Returns the record's byte offset within the shard it landed in.
    uint64_t write_record(const std::vector<uint8_t>& rec) {
        if (shardBytes_ >= cfg_.shardCapBytes) open_shard(shardIdx_ + 1);
        if (!shard_) return 0;
        uint64_t off = shardBytes_;
        std::fwrite(rec.data(), 1, rec.size(), shard_);
        shardBytes_ += rec.size();
        bytesWritten_.fetch_add(rec.size());
        return off;
    }

    uint32_t rand_u32() { rng_ ^= rng_ << 13; rng_ ^= rng_ >> 7; rng_ ^= rng_ << 17; return (uint32_t)(rng_ >> 32); }

    static inline uint8_t luma(const uint8_t* p) { return (uint8_t)((p[0]*77 + p[1]*150 + p[2]*29) >> 8); }

    // Per-cell luma SAD motion map between two same-size RGBA frames, plus the mean
    // per-pixel abs-diff (the skip/keep score, 0..255). cell = 16px grid.
    void motion_map(const CapFrame& a, const CapFrame& b, std::vector<float>& cells,
                    int& cw, int& ch, float& meanAbs) const {
        const int W = a.w, H = a.h, CELL = 16;
        cw = (W + CELL - 1) / CELL; ch = (H + CELL - 1) / CELL;
        cells.assign((size_t)cw * ch, 0.0f);
        uint64_t total = 0;
        for (int y = 0; y < H; ++y) {
            const uint8_t* ra = &a.rgba[(size_t)y * W * 4];
            const uint8_t* rb = &b.rgba[(size_t)y * W * 4];
            int cy = y / CELL;
            for (int x = 0; x < W; ++x) {
                int d = (int)luma(ra + x*4) - (int)luma(rb + x*4);
                if (d < 0) d = -d;
                cells[(size_t)cy * cw + (x / CELL)] += (float)d;
                total += (uint64_t)d;
            }
        }
        meanAbs = (W && H) ? (float)((double)total / ((double)W * H)) : 0.0f;
    }

    // Choose up to nPatches motion-rich top-left coords, spatially spread, with a
    // random tiebreak among strong candidates (variety across frames). Coords are
    // in downscaled-pixel space; a patch spans [x,x+ps) x [y,y+ps).
    void pick_patches(const std::vector<float>& cells, int cw, int ch, int W, int H, int ps,
                      std::vector<std::pair<int,int>>& out) {
        out.clear();
        const int CELL = 16;
        const int maxX = W - ps, maxY = H - ps;
        if (maxX < 0 || maxY < 0) return;
        const int stride = ps / 2 > 0 ? ps / 2 : 1;
        struct Cand { float score; int x, y; };
        std::vector<Cand> cand;
        for (int y = 0; y <= maxY; y += stride) {
            for (int x = 0; x <= maxX; x += stride) {
                float s = 0.0f;
                int cx0 = x / CELL, cy0 = y / CELL, cx1 = (x + ps - 1) / CELL, cy1 = (y + ps - 1) / CELL;
                for (int cy = cy0; cy <= cy1 && cy < ch; ++cy)
                    for (int cx = cx0; cx <= cx1 && cx < cw; ++cx)
                        s += cells[(size_t)cy * cw + cx];
                s *= 1.0f + 0.05f * ((float)(rand_u32() & 1023) / 1023.0f);  // tiny random tiebreak
                cand.push_back({s, x, y});
            }
        }
        if (cand.empty()) return;
        const int minSep = ps / 2;
        const int want = cfg_.nPatches;
        for (int k = 0; k < want && (int)out.size() < want; ++k) {
            int best = -1; float bestS = -1.0f;
            for (size_t i = 0; i < cand.size(); ++i) {
                bool far = true;
                for (auto& c : out)
                    if (std::abs(cand[i].x - c.first) < minSep && std::abs(cand[i].y - c.second) < minSep) { far = false; break; }
                if (far && cand[i].score > bestS) { bestS = cand[i].score; best = (int)i; }
            }
            if (best < 0) break;
            out.push_back({cand[best].x, cand[best].y});
            cand[best].score = -2.0f;   // consume
        }
    }

    // Copy a ps x ps crop from frame f at (x,y) into dst at column dstX (RGBA8, row pitch dstPitch).
    static void crop_into(const CapFrame& f, int x, int y, int ps, uint8_t* dst, int dstPitch, int dstX) {
        for (int r = 0; r < ps; ++r) {
            const uint8_t* srow = &f.rgba[((size_t)(y + r) * f.w + x) * 4];
            uint8_t* drow = dst + (size_t)r * dstPitch + (size_t)dstX * 4;
            std::memcpy(drow, srow, (size_t)ps * 4);
        }
    }

    // ---- frame mode: retroactive-write so every moving transition keeps BOTH ends,
    //      giving contiguous source-index runs from which offline triplets form. ----
    void process_frame_mode(CapFrame&& item) {
        if (!havePrev_) { prev_ = std::move(item); havePrev_ = true; prevWritten_ = false; return; }
        int cw, ch; std::vector<float> cells; float meanAbs = 0.0f;
        bool contiguous = (item.src == prev_.src + 1) && item.w == prev_.w && item.h == prev_.h;
        if (contiguous) motion_map(prev_, item, cells, cw, ch, meanAbs);
        if (contiguous && meanAbs >= cfg_.motionThresh) {
            if (!prevWritten_) { emit_frame(prev_, 0.0f); prevWritten_ = true; }
            emit_frame(item, meanAbs);
            prev_ = std::move(item); prevWritten_ = true;
        } else {
            if (item.src != prev_.src + 1) gaps_.fetch_add(1); else skipped_.fetch_add(1);
            prev_ = std::move(item); prevWritten_ = false;
        }
    }
    void emit_frame(const CapFrame& f, float motion) {
        std::vector<uint8_t> qoi; qoi_encode(f.rgba.data(), f.w, f.h, qoi);
        std::vector<uint8_t> rec;
        wu8(rec, 1); wu8(rec, 1); wu16(rec, 0);
        wu32(rec, (uint32_t)seq_); wu64(rec, f.src); wu64(rec, f.ts); wf32(rec, motion); wu32(rec, 0);
        wu16(rec, 0); wu16(rec, 0); wu16(rec, (uint16_t)f.w); wu16(rec, (uint16_t)f.h);
        wu32(rec, (uint32_t)qoi.size());
        uint32_t blobRel = (uint32_t)rec.size();
        rec.insert(rec.end(), qoi.begin(), qoi.end());
        uint64_t recOff = write_record(rec);
        if (manifest_) {
            std::fprintf(manifest_,
                "{\"seq\":%llu,\"src\":%llu,\"ts_ns\":%llu,\"w\":%d,\"h\":%d,\"motion\":%.3f,"
                "\"shard\":\"%s\",\"rec_off\":%llu,\"off\":%llu,\"len\":%llu}\n",
                (unsigned long long)seq_, (unsigned long long)f.src, (unsigned long long)f.ts,
                f.w, f.h, motion, curShard_.c_str(), (unsigned long long)recOff,
                (unsigned long long)(recOff + blobRel), (unsigned long long)qoi.size());
            std::fflush(manifest_);
        }
        ++seq_; written_.fetch_add(1);
    }

    // ---- patch mode: self-contained aligned triplets on a 1-frame delay. ----
    void process_patch_mode(CapFrame&& item) {
        hist_.push_back(std::move(item));
        if (hist_.size() > 3) hist_.pop_front();
        if (hist_.size() < 3) return;
        const CapFrame& A = hist_[0];   // prev  (input)
        const CapFrame& C = hist_[1];   // center(target)
        const CapFrame& B = hist_[2];   // next  (input)
        bool contiguous = (A.src + 1 == C.src) && (C.src + 1 == B.src)
                          && A.w == B.w && A.h == B.h && A.w == C.w && A.h == C.h;
        if (!contiguous) { gaps_.fetch_add(1); return; }
        int cw, ch; std::vector<float> cells; float meanAbs = 0.0f;
        motion_map(A, B, cells, cw, ch, meanAbs);           // endpoint displacement
        if (meanAbs < cfg_.motionThresh) { skipped_.fetch_add(1); return; }
        int ps = cfg_.patchSize;
        if (ps > C.w) ps = C.w; if (ps > C.h) ps = C.h;     // clamp to tiny targets
        std::vector<std::pair<int,int>> coords;
        pick_patches(cells, cw, ch, C.w, C.h, ps, coords);
        if (coords.empty()) { skipped_.fetch_add(1); return; }

        std::vector<uint8_t> rec;
        wu8(rec, 0); wu8(rec, (uint8_t)coords.size()); wu16(rec, 0);
        wu32(rec, (uint32_t)seq_); wu64(rec, C.src); wu64(rec, C.ts); wf32(rec, meanAbs); wu32(rec, 0);
        const int pitch = ps * 3 * 4;
        std::vector<uint8_t> triple((size_t)pitch * ps);
        std::vector<uint32_t> blobRel(coords.size());
        std::vector<uint32_t> blobLen(coords.size());
        for (size_t k = 0; k < coords.size(); ++k) {
            int x = coords[k].first, y = coords[k].second;
            crop_into(A, x, y, ps, triple.data(), pitch, 0);        // [prev
            crop_into(C, x, y, ps, triple.data(), pitch, ps);       //  center
            crop_into(B, x, y, ps, triple.data(), pitch, ps * 2);   //  next]
            std::vector<uint8_t> qoi; qoi_encode(triple.data(), ps * 3, ps, qoi);
            wu16(rec, (uint16_t)x); wu16(rec, (uint16_t)y);
            wu16(rec, (uint16_t)(ps * 3)); wu16(rec, (uint16_t)ps);
            wu32(rec, (uint32_t)qoi.size());
            blobRel[k] = (uint32_t)rec.size();
            blobLen[k] = (uint32_t)qoi.size();
            rec.insert(rec.end(), qoi.begin(), qoi.end());
            patchesWritten_.fetch_add(1);
        }
        uint64_t recOff = write_record(rec);
        if (manifest_) {
            std::fprintf(manifest_,
                "{\"seq\":%llu,\"src_center\":%llu,\"ts_ns\":%llu,\"motion\":%.3f,\"ps\":%d,"
                "\"shard\":\"%s\",\"rec_off\":%llu,\"patches\":[",
                (unsigned long long)seq_, (unsigned long long)C.src, (unsigned long long)C.ts,
                meanAbs, ps, curShard_.c_str(), (unsigned long long)recOff);
            for (size_t k = 0; k < coords.size(); ++k)
                std::fprintf(manifest_, "%s{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"off\":%llu,\"len\":%llu}",
                             k ? "," : "", coords[k].first, coords[k].second, ps * 3, ps,
                             (unsigned long long)(recOff + blobRel[k]), (unsigned long long)blobLen[k]);
            std::fprintf(manifest_, "]}\n");
            std::fflush(manifest_);
        }
        ++seq_; written_.fetch_add(1);
    }

    void process(CapFrame&& item) {
        if (cfg_.mode == 1) process_frame_mode(std::move(item));
        else                process_patch_mode(std::move(item));
    }

    void loop() {
        for (;;) {
            CapFrame item;
            {
                std::unique_lock<std::mutex> lk(qlock_);
                qcv_.wait(lk, [this]{ return !queue_.empty() || stop_.load(); });
                if (queue_.empty()) { if (stop_.load()) break; else continue; }
                item = std::move(queue_.front());
                queue_.pop_front();
            }
            process(std::move(item));
        }
    }

    CaptureConfig cfg_;
    std::string   sessionDir_;
    FILE*         manifest_ = nullptr;
    FILE*         shard_ = nullptr;
    std::string   curShard_;
    int           shardIdx_ = 0;
    uint64_t      shardBytes_ = 0;
    std::thread   thread_;
    std::mutex    qlock_;
    std::condition_variable qcv_;
    std::deque<CapFrame> queue_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> written_{0}, patchesWritten_{0}, skipped_{0}, gaps_{0}, drops_{0}, bytesWritten_{0};
    uint64_t seq_ = 0;
    uint64_t rng_ = 0x123456789abcdefull;
    // frame-mode carry
    CapFrame prev_; bool havePrev_ = false; bool prevWritten_ = false;
    // patch-mode 3-frame ring
    std::deque<CapFrame> hist_;
};

} // namespace winfg
