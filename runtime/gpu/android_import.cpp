// First launch of the shared Android build: the app carries no game files, so it
// finds the player's own disc image or extracted folder on the device, checks
// that it is the supported release and copies the files into the app's folder.
// Downloadable content packs found on the device are added the same way.
#ifdef __ANDROID__
#include <android/api-level.h>
#include <android/native_activity.h>
#include <dirent.h>
#include <jni.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "window.h"

namespace tje {
void hle_log(const char* fmt, ...);
}

namespace tje::gpu {

namespace {

constexpr char kXbeSha1[] = "eb00994059d1b7d357810a1d80c8878e8f14d10b";     // USA release
constexpr uint32_t kTitleId = 0x5345000F;
constexpr uint64_t kSector = 2048;
// Where the game partition starts: extracted-style images, then full disc dumps.
constexpr uint64_t kIsoOffsets[] = {0, 0x18300000, 0xFD90000, 0x2080000};
constexpr char kIsoMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr int kScanDepth = 6;

// ---------------------------------------------------------------- SHA-1

struct Sha1 {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    uint8_t block[64];
    uint64_t total = 0;
    size_t used = 0;

    static uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

    void compress() {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)block[i * 4] << 24 | (uint32_t)block[i * 4 + 1] << 16 | (uint32_t)block[i * 4 + 2] << 8 | block[i * 4 + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void add(const uint8_t* p, size_t n) {
        total += n;
        while (n) {
            size_t take = std::min(n, sizeof block - used);
            std::memcpy(block + used, p, take);
            used += take; p += take; n -= take;
            if (used == sizeof block) { compress(); used = 0; }
        }
    }

    std::string hex() {
        uint64_t bits = total * 8;
        uint8_t pad = 0x80;
        add(&pad, 1);
        pad = 0;
        while (used != 56) add(&pad, 1);
        uint8_t len[8];
        for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - 8 * i));
        add(len, 8);
        char out[41];
        for (int i = 0; i < 5; i++) std::snprintf(out + i * 8, 9, "%08x", h[i]);
        return out;
    }
};

// ---------------------------------------------------------------- files

struct Piece { std::string rel; uint64_t offset, size; };     // offset: into the disc image; unused for folders

struct Source {
    std::string path;
    bool iso = false;
    uint64_t size = 0;
    std::vector<Piece> files;
};

struct Found {
    std::vector<Source> games;
    std::map<uint32_t, std::string> packs;      // offer id -> folder
    int wrong = 0;                              // copies of something else, or of another release
};

bool same_name(const char* a, const char* b) { return strcasecmp(a, b) == 0; }

bool read_at(FILE* f, uint64_t offset, void* out, size_t n) {
    return fseeko(f, (off_t)offset, SEEK_SET) == 0 && std::fread(out, 1, n, f) == n;
}

uint32_t le32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

std::string sha1_range(FILE* f, uint64_t offset, uint64_t size) {
    Sha1 s;
    std::vector<uint8_t> buf(1 << 20);
    if (fseeko(f, (off_t)offset, SEEK_SET) != 0) return "";
    while (size) {
        size_t n = std::fread(buf.data(), 1, (size_t)std::min<uint64_t>(size, buf.size()), f);
        if (!n) return "";
        s.add(buf.data(), n);
        size -= n;
    }
    return s.hex();
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

// Every file in a disc image. False for anything that isn't a sound Xbox image.
bool iso_files(FILE* f, uint64_t image_size, std::vector<Piece>& out) {
    uint8_t vd[kSector];
    uint64_t base = 0;
    bool found = false;
    for (uint64_t offset : kIsoOffsets) {
        if (read_at(f, offset + 32 * kSector, vd, sizeof vd) && !std::memcmp(vd, kIsoMagic, 20) &&
            !std::memcmp(vd + 0x7EC, kIsoMagic, 20)) {
            base = offset;
            found = true;
            break;
        }
    }
    if (!found) return false;
    struct Dir { std::string path; uint32_t sector, size; };
    std::vector<Dir> dirs{{"", le32(vd + 20), le32(vd + 24)}};
    size_t dirs_seen = 0;
    while (!dirs.empty()) {
        Dir d = dirs.back();
        dirs.pop_back();
        if (!d.size) continue;
        if (d.size > (16u << 20) || ++dirs_seen > 20000) return false;
        std::vector<uint8_t> buf(d.size);
        if (!read_at(f, base + d.sector * kSector, buf.data(), buf.size())) return false;
        // Directory entries form a binary tree linked by dword offsets.
        std::vector<uint32_t> stack{0};
        std::vector<bool> seen(buf.size() / 4 + 1);
        while (!stack.empty()) {
            uint32_t o = stack.back();
            stack.pop_back();
            if (o + 14 > buf.size() || seen[o / 4]) continue;
            seen[o / 4] = true;
            const uint8_t* e = buf.data() + o;
            uint32_t left = e[0] | e[1] << 8, right = e[2] | e[3] << 8, start = le32(e + 4), size = le32(e + 8);
            if (left == 0xFFFF && right == 0xFFFF && start == 0xFFFFFFFF) continue;     // padding
            uint8_t attr = e[12], nlen = e[13];
            if (o + 14 + nlen > buf.size()) return false;
            std::string name((const char*)e + 14, nlen);
            if (name.empty() || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos ||
                name.find('\0') != std::string::npos)
                return false;
            std::string rel = d.path.empty() ? name : d.path + "/" + name;
            if (attr & 0x10) {
                dirs.push_back({rel, start, size});
            } else {
                uint64_t at = base + start * kSector;
                if (at + size > image_size) return false;
                out.push_back({rel, at, size});
                if (out.size() > 200000) return false;
            }
            if (left && left != 0xFFFF) stack.push_back(left * 4);
            if (right && right != 0xFFFF) stack.push_back(right * 4);
        }
    }
    return true;
}

void folder_files(const std::string& root, const std::string& rel, std::vector<Piece>& out, uint64_t& total) {
    const std::string dir = rel.empty() ? root : root + "/" + rel;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        const std::string child = rel.empty() ? e->d_name : rel + "/" + e->d_name;
        struct stat st;
        if (stat((root + "/" + child).c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            folder_files(root, child, out, total);
        } else if (S_ISREG(st.st_mode)) {
            out.push_back({child, 0, (uint64_t)st.st_size});
            total += (uint64_t)st.st_size;
        }
    }
    closedir(d);
}

void check_iso(const std::string& path, Found& found) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return;
    Source s;
    s.path = path;
    s.iso = true;
    if (iso_files(f, file_size(path), s.files)) {
        const Piece* xbe = nullptr;
        for (const Piece& p : s.files) {
            s.size += p.size;
            if (same_name(p.rel.c_str(), "default.xbe")) xbe = &p;
        }
        if (xbe && sha1_range(f, xbe->offset, xbe->size) == kXbeSha1) found.games.push_back(std::move(s));
        else found.wrong++;
    }
    std::fclose(f);
}

void check_folder(const std::string& path, const std::string& xbe_name, Found& found) {
    const std::string xbe = path + "/" + xbe_name;
    FILE* f = std::fopen(xbe.c_str(), "rb");
    if (!f) return;
    const bool ok = sha1_range(f, 0, file_size(xbe)) == kXbeSha1;
    std::fclose(f);
    if (!ok) {
        found.wrong++;
        return;
    }
    Source s;
    s.path = path;
    folder_files(path, "", s.files, s.size);
    found.games.push_back(std::move(s));
}

void check_pack(const std::string& path, const std::string& meta_name, Found& found) {
    FILE* f = std::fopen((path + "/" + meta_name).c_str(), "rb");
    if (!f) return;
    uint8_t header[0x6c];
    const bool ok = std::fread(header, 1, sizeof header, f) == sizeof header;
    std::fclose(f);
    if (ok && !std::memcmp(header + 0x14, "XCSF", 4) && le32(header + 0x24) == kTitleId) found.packs[le32(header + 0x28)] = path;
}

std::function<void(int)> g_scan_tick;           // told how many folders have been looked at

void scan(const std::string& dir, int depth, const std::vector<std::string>& skip, Found& found) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    static int folders = 0;
    if (g_scan_tick) g_scan_tick(++folders);
    std::vector<std::string> subdirs, isos;
    std::string xbe, meta;
    while (dirent* e = readdir(d)) {
        const char* n = e->d_name;
        if (n[0] == '.') continue;
        if (same_name(n, "default.xbe")) xbe = n;
        else if (same_name(n, "ContentMeta.xbx")) meta = n;
        const size_t len = std::strlen(n);
        const bool iso_name = len > 4 && same_name(n + len - 4, ".iso");
        // Asking about every file is slow on a big memory card: only where the listing doesn't say what it is.
        bool is_dir = e->d_type == DT_DIR;
        struct stat st{};
        if (e->d_type == DT_UNKNOWN || e->d_type == DT_LNK || (iso_name && !is_dir)) {
            if (stat((dir + "/" + n).c_str(), &st) != 0) continue;
            is_dir = S_ISDIR(st.st_mode);
        }
        if (is_dir) {
            if (std::find(skip.begin(), skip.end(), n) == skip.end()) subdirs.push_back(n);
        } else if (iso_name && st.st_size > (64 << 20)) {
            isos.push_back(n);
        }
    }
    closedir(d);
    if (!meta.empty()) {
        check_pack(dir, meta, found);
        return;
    }
    if (!xbe.empty()) {
        check_folder(dir, xbe, found);
        return;                                 // nothing to find inside a copy of the game
    }
    for (const std::string& n : isos) check_iso(dir + "/" + n, found);
    if (depth >= kScanDepth) return;
    for (const std::string& n : subdirs) scan(dir + "/" + n, depth + 1, {}, found);
}

Found scan_device(const std::string& files) {
    Found found;
    scan(files, 0, {"data", "DLC", "userdata"}, found);         // the app's own folder: reachable with no permission
    scan("/storage/emulated/0", 0, {"Android"}, found);
    if (DIR* d = opendir("/storage")) {                         // memory cards
        while (dirent* e = readdir(d)) {
            const std::string n = e->d_name;
            if (n[0] != '.' && n != "emulated" && n != "self") scan("/storage/" + n, 0, {"Android"}, found);
        }
        closedir(d);
    }
    hle_log("[import] %zu game copies, %zu content packs, %d other", found.games.size(), found.packs.size(), found.wrong);
    return found;
}

bool make_dirs(const std::string& path) {
    for (size_t at = 1; at <= path.size(); at++) {
        if (at != path.size() && path[at] != '/') continue;
        const std::string part = path.substr(0, at);
        struct stat st;
        if (stat(part.c_str(), &st) != 0 && mkdir(part.c_str(), 0770) != 0 && stat(part.c_str(), &st) != 0) return false;
    }
    return true;
}

// Copies one file, or a range of the disc image, to target. An interrupted file is left as .part.
bool copy_piece(FILE* in, uint64_t offset, uint64_t size, const std::string& target, std::vector<uint8_t>& buf,
                const std::function<void(uint64_t)>& copied) {
    if (file_size(target) == size && size) {    // from an earlier, interrupted run
        copied(size);
        return true;
    }
    if (!make_dirs(target.substr(0, target.rfind('/')))) return false;
    const std::string part = target + ".part";
    FILE* out = std::fopen(part.c_str(), "wb");
    if (!out) return false;
    bool ok = fseeko(in, (off_t)offset, SEEK_SET) == 0;
    while (ok && size) {
        size_t n = std::fread(buf.data(), 1, (size_t)std::min<uint64_t>(size, buf.size()), in);
        ok = n && std::fwrite(buf.data(), 1, n, out) == n;
        size -= n;
        if (ok) copied(n);
    }
    ok = std::fclose(out) == 0 && ok;
    if (!ok) {
        remove(part.c_str());
        return false;
    }
    return rename(part.c_str(), target.c_str()) == 0;
}

// The game's files, with default.xbe last: its presence marks a finished copy.
bool copy_game(const Source& s, const std::string& dest, const std::function<void(uint64_t)>& copied) {
    std::vector<uint8_t> buf(1 << 20);
    FILE* image = s.iso ? std::fopen(s.path.c_str(), "rb") : nullptr;
    if (s.iso && !image) return false;
    bool ok = true;
    for (int pass = 0; pass < 2 && ok; pass++) {
        for (const Piece& p : s.files) {
            const bool is_xbe = same_name(p.rel.c_str(), "default.xbe");
            if (is_xbe != (pass == 1)) continue;
            const std::string target = dest + "/" + (is_xbe ? "default.xbe" : p.rel);
            if (s.iso) {
                ok = copy_piece(image, p.offset, p.size, target, buf, copied);
            } else {
                FILE* in = std::fopen((s.path + "/" + p.rel).c_str(), "rb");
                ok = in && copy_piece(in, 0, p.size, target, buf, copied);
                if (in) std::fclose(in);
            }
            if (!ok) {
                hle_log("[import] failed on %s", p.rel.c_str());
                break;
            }
        }
    }
    if (image) std::fclose(image);
    return ok;
}

bool copy_pack(const std::string& from, const std::string& to, const std::function<void(uint64_t)>& copied) {
    std::vector<Piece> files;
    uint64_t total = 0;
    folder_files(from, "", files, total);
    std::vector<uint8_t> buf(1 << 20);
    for (const Piece& p : files) {
        FILE* in = std::fopen((from + "/" + p.rel).c_str(), "rb");
        const bool ok = in && copy_piece(in, 0, p.size, to + "/" + p.rel, buf, copied);
        if (in) std::fclose(in);
        if (!ok) {
            hle_log("[import] failed on %s/%s", from.c_str(), p.rel.c_str());
            return false;
        }
    }
    return true;
}

uint64_t pack_bytes(const std::string& path) {
    std::vector<Piece> files;
    uint64_t total = 0;
    folder_files(path, "", files, total);
    return total;
}

// ---------------------------------------------------------------- the screens

constexpr uint32_t kBack = 0xff101014, kText = 0xffffffff, kDim = 0xffa8a8b0, kAccent = 0xffffb020, kBar = 0xff38383f;

// Text is drawn by Android itself into a bitmap, which is then shown as a picture.
class Screen {
public:
    explicit Screen(JNIEnv* env) : env_(env) {}

    bool begin() {
        uint32_t w, h;
        if (!android_screen_size(&w, &h)) return false;
        env_->PushLocalFrame(64);
        if (w != w_ || h != h_) {
            release();
            jclass bc = env_->FindClass("android/graphics/Bitmap");
            jclass cc = env_->FindClass("android/graphics/Bitmap$Config");
            jobject cfg = env_->GetStaticObjectField(cc, env_->GetStaticFieldID(cc, "ARGB_8888", "Landroid/graphics/Bitmap$Config;"));
            jobject bmp = env_->CallStaticObjectMethod(
                bc, env_->GetStaticMethodID(bc, "createBitmap", "(IILandroid/graphics/Bitmap$Config;)Landroid/graphics/Bitmap;"),
                (jint)w, (jint)h, cfg);
            jclass vc = env_->FindClass("android/graphics/Canvas");
            jobject canvas = env_->NewObject(vc, env_->GetMethodID(vc, "<init>", "(Landroid/graphics/Bitmap;)V"), bmp);
            jclass pc = env_->FindClass("android/graphics/Paint");
            jobject paint = env_->NewObject(pc, env_->GetMethodID(pc, "<init>", "(I)V"), 1);      // ANTI_ALIAS_FLAG
            if (env_->ExceptionCheck() || !bmp || !canvas || !paint) {
                env_->ExceptionClear();
                env_->PopLocalFrame(nullptr);
                return false;
            }
            bmp_ = env_->NewGlobalRef(bmp);
            canvas_ = env_->NewGlobalRef(canvas);
            paint_ = env_->NewGlobalRef(paint);
            w_ = w;
            h_ = h;
            px_.assign((size_t)w * h * 4, 0);
        }
        jclass vc = env_->GetObjectClass(canvas_);
        env_->CallVoidMethod(canvas_, env_->GetMethodID(vc, "drawColor", "(I)V"), (jint)kBack);
        return true;
    }

    float unit() const { return h_ / 18.0f; }
    float width() const { return (float)w_; }
    float height() const { return (float)h_; }

    // Centred on x; y is the baseline.
    void text(const std::string& s, float x, float y, float size, uint32_t color) {
        std::string ascii = s;
        for (char& c : ascii) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) c = '?';
        jclass pc = env_->GetObjectClass(paint_);
        jclass ac = env_->FindClass("android/graphics/Paint$Align");
        jobject center = env_->GetStaticObjectField(ac, env_->GetStaticFieldID(ac, "CENTER", "Landroid/graphics/Paint$Align;"));
        env_->CallVoidMethod(paint_, env_->GetMethodID(pc, "setTextAlign", "(Landroid/graphics/Paint$Align;)V"), center);
        env_->CallVoidMethod(paint_, env_->GetMethodID(pc, "setTextSize", "(F)V"), (jfloat)size);
        env_->CallVoidMethod(paint_, env_->GetMethodID(pc, "setColor", "(I)V"), (jint)color);
        jstring js = env_->NewStringUTF(ascii.c_str());
        jclass vc = env_->GetObjectClass(canvas_);
        env_->CallVoidMethod(canvas_, env_->GetMethodID(vc, "drawText", "(Ljava/lang/String;FFLandroid/graphics/Paint;)V"), js,
                             (jfloat)x, (jfloat)y, paint_);
        env_->DeleteLocalRef(js);
        env_->DeleteLocalRef(center);
        env_->DeleteLocalRef(ac);
        env_->DeleteLocalRef(pc);
        env_->DeleteLocalRef(vc);
    }

    void rect(float l, float t, float r, float b, uint32_t color) {
        jclass pc = env_->GetObjectClass(paint_);
        env_->CallVoidMethod(paint_, env_->GetMethodID(pc, "setColor", "(I)V"), (jint)color);
        jclass vc = env_->GetObjectClass(canvas_);
        env_->CallVoidMethod(canvas_, env_->GetMethodID(vc, "drawRect", "(FFFFLandroid/graphics/Paint;)V"), (jfloat)l, (jfloat)t,
                             (jfloat)r, (jfloat)b, paint_);
        env_->DeleteLocalRef(pc);
        env_->DeleteLocalRef(vc);
    }

    void end() {
        jobject buffer = env_->NewDirectByteBuffer(px_.data(), (jlong)px_.size());
        jclass bc = env_->GetObjectClass(bmp_);
        env_->CallVoidMethod(bmp_, env_->GetMethodID(bc, "copyPixelsToBuffer", "(Ljava/nio/Buffer;)V"), buffer);
        if (env_->ExceptionCheck()) env_->ExceptionClear();
        env_->PopLocalFrame(nullptr);
        android_show_pixels(px_.data(), w_, h_);
    }

    void release() {
        if (bmp_) env_->DeleteGlobalRef(bmp_);
        if (canvas_) env_->DeleteGlobalRef(canvas_);
        if (paint_) env_->DeleteGlobalRef(paint_);
        bmp_ = canvas_ = paint_ = nullptr;
        w_ = h_ = 0;
    }

private:
    JNIEnv* env_;
    jobject bmp_ = nullptr, canvas_ = nullptr, paint_ = nullptr;
    uint32_t w_ = 0, h_ = 0;
    std::vector<uint8_t> px_;
};

struct Page {
    std::string title;
    std::vector<std::string> lines;             // body text
    std::vector<std::string> choices;           // a list to pick from, under the text
    int chosen = 0;
    std::string hint;                           // what the buttons do
    float progress = -1;                        // 0..1 shows a bar
};

// Draws a page; returns the top and height of the list rows for touch.
void draw(Screen& s, const Page& p, float* list_top = nullptr, float* row_h = nullptr) {
    if (!s.begin()) return;
    const float u = s.unit(), cx = s.width() / 2;
    float y = 3.2f * u;
    s.text(p.title, cx, y, 1.25f * u, kAccent);
    y += 2.0f * u;
    for (const std::string& line : p.lines) {
        s.text(line, cx, y, 0.72f * u, kText);
        y += 1.05f * u;
    }
    y += 0.3f * u;
    if (list_top) *list_top = y - 0.85f * u;
    if (row_h) *row_h = 1.2f * u;
    for (size_t i = 0; i < p.choices.size(); i++) {
        const bool on = (int)i == p.chosen;
        if (on && p.choices.size() > 1) s.rect(cx - 0.42f * s.width(), y - 0.85f * u, cx + 0.42f * s.width(), y + 0.35f * u, kBar);
        s.text(p.choices[i], cx, y, 0.72f * u, on ? kAccent : kDim);
        y += 1.2f * u;
    }
    if (p.progress >= 0) {
        const float l = cx - 0.35f * s.width(), r = cx + 0.35f * s.width(), t = 11.0f * u, b = 11.6f * u;
        s.rect(l, t, r, b, kBar);
        s.rect(l, t, l + (r - l) * std::min(p.progress, 1.0f), b, kAccent);
    }
    if (!p.hint.empty()) s.text(p.hint, cx, s.height() - 1.6f * u, 0.72f * u, kDim);
    s.end();
}

enum class Press { None, Confirm, Back };

// Shows the page until the player confirms (A, Start or a tap) or goes back (B). keep_waiting ends it early when it returns false.
Press wait_inner(Screen& s, Page& p, const std::function<bool()>& keep_waiting) {
    uint32_t before = ~0u;                      // buttons held on entry don't count
    float tx, ty;
    android_take_tap(&tx, &ty);
    android_take_pressed();
    auto drawn = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    bool dirty = true;
    float list_top = 0, row_h = 1;
    for (;;) {
        window_wait_foreground();
        auto now = std::chrono::steady_clock::now();
        if (dirty || now - drawn > std::chrono::milliseconds(300)) {
            draw(s, p, &list_top, &row_h);
            drawn = now;
            dirty = false;
            if (keep_waiting && !keep_waiting()) return Press::None;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        // Presses are collected as they happen, so a quick one between two looks isn't missed.
        const uint32_t keys = window_keys(), pressed = (android_take_pressed() | keys) & ~before;
        before = keys;
        // Either face button: handhelds disagree on which of the two is called A.
        if (pressed & (KEY_A | KEY_B | KEY_START)) return Press::Confirm;
        const int count = (int)p.choices.size();
        if (count > 1 && (pressed & KEY_UP)) { p.chosen = (p.chosen + count - 1) % count; dirty = true; }
        if (count > 1 && (pressed & KEY_DOWN)) { p.chosen = (p.chosen + 1) % count; dirty = true; }
        if (android_take_tap(&tx, &ty)) {
            const int row = ty >= list_top ? (int)((ty - list_top) / row_h) : -1;
            if (count > 1 && row >= 0 && row < count) p.chosen = row;      // a tap on a row picks it
            return Press::Confirm;
        }
    }
}

Press wait(Screen& s, Page& p, const std::function<bool()>& keep_waiting = nullptr) {
    const Press r = wait_inner(s, p, keep_waiting);
    hle_log("[import] \"%s\": %s, choice %d", p.title.c_str(), r == Press::Confirm ? "confirm" : r == Press::Back ? "back" : "done", p.chosen);
    return r;
}

// ---------------------------------------------------------------- access to the device's files

bool has_access(JNIEnv* env, ANativeActivity* activity) {
    env->PushLocalFrame(16);
    bool ok = false;
    if (android_get_device_api_level() >= 30) {
        jclass c = env->FindClass("android/os/Environment");
        ok = env->CallStaticBooleanMethod(c, env->GetStaticMethodID(c, "isExternalStorageManager", "()Z"));
    } else {
        jclass c = env->GetObjectClass(activity->clazz);
        jstring name = env->NewStringUTF("android.permission.READ_EXTERNAL_STORAGE");
        ok = env->CallIntMethod(activity->clazz, env->GetMethodID(c, "checkSelfPermission", "(Ljava/lang/String;)I"), name) == 0;
    }
    if (env->ExceptionCheck()) { env->ExceptionClear(); ok = false; }
    env->PopLocalFrame(nullptr);
    return ok;
}

// Opens the system's own screen for it; the player decides there.
void ask_access(JNIEnv* env, ANativeActivity* activity) {
    env->PushLocalFrame(32);
    jclass ac = env->GetObjectClass(activity->clazz);
    if (android_get_device_api_level() >= 30) {
        jstring pkg = (jstring)env->CallObjectMethod(activity->clazz, env->GetMethodID(ac, "getPackageName", "()Ljava/lang/String;"));
        const char* chars = env->GetStringUTFChars(pkg, nullptr);
        const std::string uri = std::string("package:") + chars;
        env->ReleaseStringUTFChars(pkg, chars);
        jclass uc = env->FindClass("android/net/Uri");
        jobject data = env->CallStaticObjectMethod(uc, env->GetStaticMethodID(uc, "parse", "(Ljava/lang/String;)Landroid/net/Uri;"),
                                                   env->NewStringUTF(uri.c_str()));
        jclass ic = env->FindClass("android/content/Intent");
        jmethodID start = env->GetMethodID(ac, "startActivity", "(Landroid/content/Intent;)V");
        jobject intent = env->NewObject(ic, env->GetMethodID(ic, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V"),
                                        env->NewStringUTF("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION"), data);
        env->CallVoidMethod(activity->clazz, start, intent);
        if (env->ExceptionCheck()) {            // no per-app screen on this device: the list of all apps instead
            env->ExceptionClear();
            intent = env->NewObject(ic, env->GetMethodID(ic, "<init>", "(Ljava/lang/String;)V"),
                                    env->NewStringUTF("android.settings.MANAGE_ALL_FILES_ACCESS_PERMISSION"));
            env->CallVoidMethod(activity->clazz, start, intent);
        }
    } else {
        jobjectArray names = env->NewObjectArray(1, env->FindClass("java/lang/String"),
                                                 env->NewStringUTF("android.permission.READ_EXTERNAL_STORAGE"));
        env->CallVoidMethod(activity->clazz, env->GetMethodID(ac, "requestPermissions", "([Ljava/lang/String;I)V"), names, 1);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    env->PopLocalFrame(nullptr);
}

std::string shown_path(std::string p) {
    const std::string internal = "/storage/emulated/0";
    if (p.rfind(internal, 0) == 0) p = "Internal storage" + p.substr(internal.size());
    else if (p.rfind("/storage/", 0) == 0) p = "Memory card" + p.substr(p.find('/', 9) == std::string::npos ? p.size() : p.find('/', 9));
    if (p.size() > 64) p = p.substr(0, 28) + " ... " + p.substr(p.size() - 31);
    return p;
}

std::string gigabytes(uint64_t bytes) {
    char out[32];
    std::snprintf(out, sizeof out, "%.1f GB", (double)bytes / (1 << 30));
    return out;
}

}  // namespace

bool android_import_game(const std::string& files) {
    if (file_size(files + "/data/default.xbe")) return true;
    ANativeActivity* activity = (ANativeActivity*)android_activity();
    JNIEnv* env = nullptr;
    if (!activity || activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
    hle_log("[import] no game files yet: starting setup");
    Screen screen(env);
    bool asked = false;
    for (;;) {
        if (!asked && !has_access(env, activity)) {
            Page intro;
            intro.title = "Set up the game";
            intro.lines = {"This app needs your own copy of the game: the USA Xbox disc,",
                           "as an .iso file or an extracted folder, anywhere on this device.",
                           "",
                           "To find it, allow access to files on the next screen,",
                           "then come back here."};
            intro.hint = "Press a button or tap to continue";
            wait(screen, intro);
            ask_access(env, activity);
            asked = true;
            Page waiting;
            waiting.title = "Set up the game";
            waiting.lines = {"Waiting for access to files.", "", "Turn it on for ToeJam & Earl III, then come back here."};
            waiting.hint = "Press a button or tap to search without it";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            wait(screen, waiting, [&] { return !has_access(env, activity); });
        }
        Page searching;
        searching.title = "Set up the game";
        searching.lines = {"Searching this device for your copy of the game..."};
        draw(screen, searching);
        auto ticked = std::chrono::steady_clock::now();
        g_scan_tick = [&](int folders) {
            auto now = std::chrono::steady_clock::now();
            if (now - ticked < std::chrono::milliseconds(400)) return;
            ticked = now;
            searching.lines.resize(1);
            searching.lines.push_back("");
            searching.lines.push_back(std::to_string(folders) + " folders checked");
            draw(screen, searching);
        };
        Found found = scan_device(files);
        g_scan_tick = nullptr;
        if (found.games.empty()) {
            Page none;
            none.title = "No copy of the game found";
            if (found.wrong) none.lines = {"A disc image or game folder was found, but it isn't the USA Xbox", "release of ToeJam & Earl III, the only one that works.", ""};
            none.lines.insert(none.lines.end(), {"Copy your .iso file or extracted game folder to this device's",
                                                 "storage or memory card, then search again."});
            if (!has_access(env, activity)) {
                none.lines.insert(none.lines.end(), {"", "Without access to files, only this folder is searched:",
                                                     "Android > data > com.orkosoftware.tje3 > files"});
                asked = false;
            }
            none.hint = "Press a button or tap to search again";
            wait(screen, none);
            continue;
        }
        Page pick;
        pick.title = "Step 1 of 2: your game";
        for (const Source& s : found.games) pick.choices.push_back(shown_path(s.path));
        pick.lines = {pick.choices.size() > 1 ? "These copies of the game were found. Choose the one to use:" : "This copy of the game was found:"};
        pick.hint = pick.choices.size() > 1 ? "Up and Down to choose, a button to continue, or tap one" : "Press a button or tap to continue";
        wait(screen, pick);
        const Source& source = found.games[(size_t)pick.chosen];

        Page extra;
        extra.title = "Step 2 of 2: downloadable content";
        if (found.packs.empty()) {
            extra.lines = {"No downloadable content was found on this device.", "",
                           "To add it, copy your DLC folders to this device's storage",
                           "or memory card, then search again."};
            extra.choices = {"Continue without it", "Search again"};
        } else {
            char count[64];
            std::snprintf(count, sizeof count, "%zu pack%s found:", found.packs.size(), found.packs.size() == 1 ? "" : "s");
            extra.lines = {count};
            std::string names;
            for (const auto& [offer, path] : found.packs) {
                const char* name = offer == 1 ? "Suteki" : offer == 2 ? "EarlBot" : offer == 3 ? "GeekJam" :
                                   offer == 0x10 ? "Hades Environment" : offer == 0x20 ? "Funkadelic Environment" : "Other content";
                if (names.size() > 40) { extra.lines.push_back(names); names.clear(); }
                names += (names.empty() ? "" : ", ") + std::string(name);
            }
            extra.lines.push_back(names);
            extra.lines.push_back("in " + shown_path(found.packs.begin()->second.substr(0, found.packs.begin()->second.rfind('/'))));
            extra.choices = {"Add them to the game", "Leave them out", "Search again"};
        }
        extra.hint = "Up and Down to choose, a button to continue, or tap one";
        wait(screen, extra);
        if (extra.choices[(size_t)extra.chosen] == "Search again") continue;
        if (extra.choices[(size_t)extra.chosen] == "Leave them out") found.packs.clear();
        uint64_t pack_total = 0;
        for (const auto& [offer, path] : found.packs) pack_total += pack_bytes(path);
        const uint64_t need = source.size + pack_total;
        struct statvfs vfs;
        const uint64_t free_bytes = statvfs(files.c_str(), &vfs) == 0 ? (uint64_t)vfs.f_bavail * vfs.f_frsize : ~0ull;
        if (free_bytes < need + (256ull << 20)) {
            Page full;
            full.title = "Not enough free space";
            full.lines = {"The game needs " + gigabytes(need + (256ull << 20)) + " free on this device's internal storage.",
                          "It has " + gigabytes(free_bytes) + ". Free some space, then try again."};
            full.hint = "Press a button or tap to try again";
            wait(screen, full);
            continue;
        }
        Page copying;
        copying.title = "Copying the game";
        char with[64];
        std::snprintf(with, sizeof with, "with %zu downloadable content pack%s", found.packs.size(), found.packs.size() == 1 ? "" : "s");
        copying.lines = {"From " + shown_path(source.path), found.packs.empty() ? "without downloadable content" : with, "",
                         "Keep the app open. This takes a few minutes."};
        copying.progress = 0;
        uint64_t done = 0;
        auto drawn = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        auto copied = [&](uint64_t n) {
            done += n;
            auto now = std::chrono::steady_clock::now();
            if (now - drawn < std::chrono::milliseconds(250)) return;
            drawn = now;
            copying.progress = need ? (float)((double)done / (double)need) : 1.0f;
            draw(screen, copying);
        };
        bool ok = true;
        for (const auto& [offer, path] : found.packs) {
            char name[20];
            std::snprintf(name, sizeof name, "%08X%08X", kTitleId, offer);
            ok = ok && copy_pack(path, files + "/DLC/" + name, copied);
        }
        ok = ok && copy_game(source, files + "/data", copied);
        if (ok) break;
        Page failed;
        failed.title = "The copy didn't finish";
        failed.lines = {"A file couldn't be read or written. Check that the device has free space",
                        "and that your copy of the game is complete, then try again."};
        failed.hint = "Press a button or tap to try again";
        wait(screen, failed);
    }
    hle_log("[import] done");
    Page starting;
    starting.title = "Starting the game";
    draw(screen, starting);
    screen.release();
    activity->vm->DetachCurrentThread();
    return true;
}

}  // namespace tje::gpu
#endif
