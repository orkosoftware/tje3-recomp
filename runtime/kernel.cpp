// HLE of the Xbox kernel (xboxkrnl.exe) exports default.xbe imports.
//
// Every import is either a function (stdcall unless noted) or a data export.
// The loader writes a fake address into each kernel thunk slot: functions get
// an address in kKernelThunks that dispatches here, data exports get guest
// memory initialised here.
#include <chrono>
#include <cerrno>
#include <cstdarg>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>

#include "kobj.h"

#ifndef _WIN32
#define _fseeki64 fseeko
#endif

namespace fs = std::filesystem;

namespace tje {

// --- status codes -------------------------------------------------------------------
enum : uint32_t {
    STATUS_SUCCESS = 0, STATUS_USER_APC = 0xc0, STATUS_TIMEOUT = 0x102, STATUS_PENDING = 0x103,
    STATUS_NO_MORE_FILES = 0x80000006, STATUS_UNSUCCESSFUL = 0xC0000001,
    STATUS_NOT_IMPLEMENTED = 0xC0000002, STATUS_INVALID_HANDLE = 0xC0000008,
    STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_END_OF_FILE = 0xC0000011,
    STATUS_NO_MEMORY = 0xC0000017, STATUS_ACCESS_DENIED = 0xC0000022,
    STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_OBJECT_NAME_COLLISION = 0xC0000035,
    STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A, STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047,
    STATUS_MUTANT_NOT_OWNED = 0xC0000046, STATUS_NOT_A_DIRECTORY = 0xC0000103,
};

static std::string g_data_root = "original/xiso_extracted";
static std::string g_user_root = "userdata";
void kernel_set_paths(const std::string& data, const std::string& user) { g_data_root = data; g_user_root = user; }

static const char kHDKey[17] = "TJE3-RECOMP-HDK!";

// Downloaded content is kept apart from the saves, in an optional folder the
// player fills: <dlc>/<offer id>/... stands in for T:\$C\<offer id>\... and is
// never written to. The game finds it by listing T:\, so $C has to exist there.
static std::string g_dlc_root;
void kernel_set_dlc(const std::string& dir) {
    g_dlc_root = dir;
    std::error_code ec;
    fs::create_directories(dir, ec);
    int packs = 0;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_directory(ec)) packs++;
    if (packs) fs::create_directories(g_user_root + "/E/TDATA/5345000f/$C", ec);
    hle_log("[dlc] %d pack%s in %s", packs, packs == 1 ? "" : "s", dir.c_str());
}
static void stamp_content_header(uint8_t* header);
static bool is_read_only(const std::string& p) {
    return p.rfind(g_data_root, 0) == 0 || (!g_dlc_root.empty() && p.rfind(g_dlc_root, 0) == 0);
}

static int64_t g_boot_100ns = 0;
static uint32_t g_tick_count_addr = 0;

// --- helpers ----------------------------------------------------------------------------
static int64_t R64s(uint32_t a) { return (int64_t)R64(a); }
static void W64s(uint32_t a, int64_t v) { W64(a, (uint64_t)v); }

// Timeout pointer -> relative 100 ns, or -1 for infinite.
static int64_t timeout_of(uint32_t p) {
    if (!p) return -1;
    int64_t t = R64s(p);
    if (t < 0) return -t;
    // absolute system time: compare with KeQuerySystemTime's clock
    using namespace std::chrono;
    int64_t now_ft = duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100
                     + 116444736000000000ll;
    return t > now_ft ? t - now_ft : 0;
}

static std::string ansi_string(uint32_t pstr) {
    if (!pstr) return "";
    uint16_t len = R16(pstr);
    uint32_t buf = R32(pstr + 4);
    return std::string(gptr(buf), len);
}

static std::string lower(std::string s) {
    for (auto& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

// Symbolic links created by the title (IoCreateSymbolicLink), e.g. "\??\d:" -> device path.
static std::map<std::string, std::string> g_links;

// Guest path -> host path. Returns "" if the device is unknown.
// The title's paths come in any case, and only Windows ignores case everywhere:
// elsewhere each missing component is looked up without regard to case.
static std::string match_case(std::string base, const std::string& rest) {
#ifndef _WIN32
    size_t at = 0;
    while (at < rest.size()) {
        size_t end = rest.find('/', at);
        if (end == std::string::npos) end = rest.size();
        std::string part = rest.substr(at, end - at), want = lower(part);
        std::error_code ec;
        if (!part.empty() && !fs::exists(base + "/" + part, ec))
            for (const auto& e : fs::directory_iterator(base, ec)) {
                std::string name = e.path().filename().string();
                if (lower(name) == want) { part = name; break; }
            }
        if (!part.empty()) base += "/" + part;
        at = end + 1;
    }
    return base;
#else
    return rest.empty() ? base : base + "/" + rest;
#endif
}

static std::string host_path(std::string p) {
    for (auto& ch : p) if (ch == '/') ch = '\\';
    std::string l = lower(p);
    if (l.rfind("\\??\\", 0) == 0) { p = p.substr(4); l = l.substr(4); }
    // resolve title-defined links on the "x:" prefix
    if (l.size() >= 2 && l[1] == ':') {
        auto it = g_links.find(l.substr(0, 2));
        if (it != g_links.end()) { p = it->second + p.substr(2); l = lower(p); }
    }
    std::string root, rest;
    auto starts = [&](const char* pre) { return l.rfind(pre, 0) == 0; };
    if (starts("\\device\\cdrom0")) { root = g_data_root; rest = p.substr(14); }
    else if (starts("\\device\\harddisk0\\partition1")) { root = g_user_root + "/E"; rest = p.substr(28); }
    else if (starts("\\device\\harddisk0\\partition")) {
        root = g_user_root + "/P" + p.substr(27, 1); rest = p.substr(28);
    }
    else if (l.size() >= 2 && l[1] == ':') {
        char d = l[0];
        if (d == 'd') root = g_data_root;
        else root = g_user_root + "/" + (char)std::toupper(d);
        rest = p.substr(2);
    } else return "";
    for (auto& ch : rest) if (ch == '\\') ch = '/';
    while (!rest.empty() && rest[0] == '/') rest.erase(0, 1);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    static const std::string kContent = "tdata/5345000f/$c";
    if (!g_dlc_root.empty() && root == g_user_root + "/E" && lower(rest).rfind(kContent, 0) == 0 &&
        (rest.size() == kContent.size() || rest[kContent.size()] == '/'))
        return match_case(g_dlc_root, rest.size() > kContent.size() ? rest.substr(kContent.size() + 1) : "");
    if (root != g_data_root) {             // writable devices exist from the start, like formatted partitions
        std::error_code ec;
        fs::create_directories(root, ec);
    }
    return match_case(root, rest);
}

static std::string object_name(uint32_t objattr) {
    std::string name = ansi_string(R32(objattr + 4));
    uint32_t root = R32(objattr);
    if (root) {
        KObject* o = handle_get(root);
        if (o) name = o->guest_path + (name.empty() || name[0] == '\\' ? "" : "\\") + name;
    }
    return name;
}

static void iosb(uint32_t p, uint32_t status, uint32_t info) {
    if (p) { W32(p, status); W32(p + 4, info); }
}

static uint64_t to_filetime(fs::file_time_type t) {
    using namespace std::chrono;
    auto sys = time_point_cast<nanoseconds>(t - fs::file_time_type::clock::now() + system_clock::now());
    return (uint64_t)(sys.time_since_epoch().count() / 100) + 116444736000000000ull;
}

static void write_network_open_info(uint32_t p, const std::string& path) {
    std::error_code ec;
    bool dir = fs::is_directory(path, ec);
    uint64_t size = dir ? 0 : (uint64_t)fs::file_size(path, ec);
    uint64_t ft = to_filetime(fs::last_write_time(path, ec));
    for (int i = 0; i < 4; i++) W64(p + 8 * i, ft);
    W64(p + 0x20, (size + 0xfff) & ~0xfffull);
    W64(p + 0x28, size);
    W32(p + 0x30, dir ? 0x10 : 0x80);       // FILE_ATTRIBUTE_DIRECTORY / NORMAL
}

// --- files ---------------------------------------------------------------------------------
// Saved games are rare and worth a record: their file activity is always logged.
static bool is_save_path(const std::string& path) {
    return path.find("/UDATA/") != std::string::npos || path.find("/TDATA/") != std::string::npos;
}

static uint32_t open_file(uint32_t handle_out, uint32_t objattr, uint32_t iosb_p, uint32_t access,
                          uint32_t disposition, uint32_t options) {
    std::string guest = object_name(objattr);
    std::string path = host_path(guest);
    if (log_enabled("file") || is_save_path(path)) hle_log("[file] open %s -> %s (disp %u opt %x)", guest.c_str(), path.c_str(),
                                     disposition, options);
    if (path.empty()) { iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0); return STATUS_OBJECT_PATH_NOT_FOUND; }
    static const char* bt_path = std::getenv("TJE_BT_PATH");      // debugging: who opens this path?
    if (bt_path && guest.find(bt_path) != std::string::npos) {
        static int n = 0, most = std::getenv("TJE_BT_MAX") ? std::atoi(std::getenv("TJE_BT_MAX")) : 3;
        if (n++ < most) { void log_backtrace(Cpu*, const char*); log_backtrace(current_cpu(), guest.c_str()); }
    }
    std::error_code ec;
    bool exists = fs::exists(path, ec);
    bool is_dir = exists && fs::is_directory(path, ec);
    bool want_dir = options & 1;
    uint32_t info = 1;      // FILE_OPENED
    if (want_dir && !exists && (disposition == 2 || disposition == 3)) {
        if (!fs::create_directory(path, ec)) {
            iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        exists = is_dir = true;
        info = 2;
    }
    if (!exists) {
        if (disposition == 1 || disposition == 4) {          // FILE_OPEN / FILE_OVERWRITE
            fs::path parent = fs::path(path).parent_path();
            uint32_t st = fs::exists(parent, ec) ? STATUS_OBJECT_NAME_NOT_FOUND : STATUS_OBJECT_PATH_NOT_FOUND;
            iosb(iosb_p, st, 5);
            return st;
        }
        if (is_read_only(path)) { iosb(iosb_p, STATUS_ACCESS_DENIED, 0); return STATUS_ACCESS_DENIED; }
        // Like NT, never create missing folders on the way: XAPI probes for a saved
        // game by opening <folder>\SaveMeta.xbx and must see "path not found", or
        // it later finds its new save folder already there and gives up.
        if (!fs::exists(fs::path(path).parent_path(), ec)) {
            if (log_enabled("file") || is_save_path(path)) hle_log("[file] no folder for %s", path.c_str());
            iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) { iosb(iosb_p, STATUS_OBJECT_PATH_NOT_FOUND, 0); return STATUS_OBJECT_PATH_NOT_FOUND; }
        std::fclose(f);
        info = 2;           // FILE_CREATED
    } else if (disposition == 2 && info != 2) {             // FILE_CREATE on something that already existed
        iosb(iosb_p, STATUS_OBJECT_NAME_COLLISION, 4);
        return STATUS_OBJECT_NAME_COLLISION;
    }
    auto* o = new KObject();
    o->type = ObjType::File;
    o->host_path = path;
    o->guest_path = guest;
    o->is_dir = is_dir;
    if (!is_dir) {
        bool truncate = exists && (disposition == 0 || disposition == 4 || disposition == 5);
        bool writable = !is_read_only(path);
        if (truncate && writable) { FILE* f = std::fopen(path.c_str(), "wb"); if (f) std::fclose(f); info = 3; }
        o->fp = std::fopen(path.c_str(), writable ? "r+b" : "rb");
        if (!o->fp) o->fp = std::fopen(path.c_str(), "rb");
        if (!o->fp) hle_log("[file] cannot open %s: %s (%zu files open)", path.c_str(), std::strerror(errno),
                            handle_open_files());
    }
    // A pack's ContentMeta header reads as a console would have left it at install.
    if (!is_dir && o->fp && !g_dlc_root.empty() && path.rfind(g_dlc_root, 0) == 0 &&
        lower(fs::path(path).filename().string()) == "contentmeta.xbx") {
        o->read_overlay.resize(0x6c);
        if (std::fread(o->read_overlay.data(), 1, 0x6c, o->fp) == 0x6c && !std::memcmp(&o->read_overlay[0x14], "XCSF", 4))
            stamp_content_header(o->read_overlay.data());
        else
            o->read_overlay.clear();
        std::rewind(o->fp);
    }
    (void)access;
    W32(handle_out, handle_new(o));
    iosb(iosb_p, STATUS_SUCCESS, info);
    return STATUS_SUCCESS;
}

static void NtCreateFile(Cpu* c) {
    RET(c, open_file(A(c, 0), A(c, 2), A(c, 3), A(c, 1), A(c, 7), A(c, 8)), 9);
}
static void NtOpenFile(Cpu* c) {
    RET(c, open_file(A(c, 0), A(c, 2), A(c, 3), A(c, 1), 1, A(c, 5)), 6);
}

// --- user-mode APCs --------------------------------------------------------------------------
static void queue_apc(GuestThread* t, uint32_t routine, uint32_t context, uint32_t arg1, uint32_t arg2) {
    {
        std::lock_guard<std::mutex> lk(t->apc_m);
        t->apcs.push_back({routine, context, arg1, arg2});
    }
    sched_wake_all();
}
static bool apc_pending(GuestThread* t) {
    std::lock_guard<std::mutex> lk(t->apc_m);
    return !t->apcs.empty();
}
// Runs the current thread's queued APCs (it is in an alertable wait).
static void deliver_apcs(Cpu* c) {
    GuestThread* t = current_thread();
    for (;;) {
        GuestThread::Apc a;
        {
            std::lock_guard<std::mutex> lk(t->apc_m);
            if (t->apcs.empty()) return;
            a = t->apcs.front();
            t->apcs.pop_front();
        }
        if (log_enabled("apc")) hle_log("[apc] t%u runs %08x(%08x, %08x, %08x)", t->id, a.routine, a.context, a.arg1, a.arg2);
        guest_call(c, a.routine, {a.context, a.arg1, a.arg2});
    }
}

static uint32_t rw_file(Cpu* c, bool write) {
    KObject* o = handle_get(A(c, 0));
    uint32_t event = A(c, 1), iosb_p = A(c, 4), buf = A(c, 5), len = A(c, 6), offp = A(c, 7);
    if (!o || !o->fp) {
        hle_log("[io] %s on %s without an open file", write ? "write" : "read", o ? o->guest_path.c_str() : "a bad handle");
        iosb(iosb_p, STATUS_INVALID_HANDLE, 0);
        return STATUS_INVALID_HANDLE;
    }
    if (offp) {
        int64_t off = R64s(offp);
        if (off >= 0) o->pos = (uint64_t)off;            // -2: FILE_USE_FILE_POINTER_POSITION
    }
    _fseeki64(o->fp, (int64_t)o->pos, SEEK_SET);
    size_t n = write ? std::fwrite(gptr(buf), 1, len, o->fp) : std::fread(gptr(buf), 1, len, o->fp);
    if (write) std::fflush(o->fp);
    if (!write && o->pos < o->read_overlay.size()) {
        size_t k = std::min<size_t>(n, o->read_overlay.size() - (size_t)o->pos);
        std::memcpy(gptr(buf), o->read_overlay.data() + o->pos, k);
    }
    o->pos += n;
    uint32_t st = (!write && n == 0 && len > 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    iosb(iosb_p, st, (uint32_t)n);
    if (event) { KObject* e = handle_get(event); if (e) { e->signaled = true; sched_wake_all(); } }
    if (A(c, 2)) queue_apc(current_thread(), A(c, 2), A(c, 3), iosb_p, 0);   // completion APC (ReadFileEx)
    if (log_enabled("io") || is_save_path(o->host_path)) hle_log("[io] %s %s @%llu len %u -> %zu (event %08x apc %08x)", write ? "write" : "read",
                                    o->guest_path.c_str(), (unsigned long long)(o->pos - n), len, n, event, A(c, 2));
    if (log_enabled("iobt")) { void log_backtrace(Cpu*, const char*); log_backtrace(c, "io"); }
    return st;
}
static void NtReadFile(Cpu* c) { RET(c, rw_file(c, false), 8); }
static void NtWriteFile(Cpu* c) { RET(c, rw_file(c, true), 8); }

static void NtQueryInformationFile(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    uint32_t iosb_p = A(c, 1), info = A(c, 2), len = A(c, 3), cls = A(c, 4);
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 5); return; }
    std::memset(gptr(info), 0, len);
    std::error_code ec;
    uint64_t size = o->is_dir ? 0 : (uint64_t)fs::file_size(o->host_path, ec);
    switch (cls) {
    case 14: W64(info, o->pos); iosb(iosb_p, 0, 8); break;                 // FilePositionInformation
    case 5:                                                                  // FileStandardInformation
        W64(info, (size + 0xfff) & ~0xfffull); W64(info + 8, size); W32(info + 16, 1);
        W8(info + 21, o->is_dir ? 1 : 0); iosb(iosb_p, 0, 24); break;
    case 34: write_network_open_info(info, o->host_path); iosb(iosb_p, 0, 0x38); break;
    case 6:                                                                  // FileInternalInformation
        W64(info, (uint64_t)std::hash<std::string>()(o->host_path)); iosb(iosb_p, 0, 8); break;
    case 9: {                                                                // FileNameInformation
        std::string n = o->guest_path;
        uint32_t k = (uint32_t)std::min<size_t>(n.size(), len > 4 ? len - 4 : 0);
        W32(info, k); std::memcpy(gptr(info + 4), n.data(), k); iosb(iosb_p, 0, 4 + k); break;
    }
    case 4: {                                                                // FileBasicInformation
        uint64_t ft = to_filetime(fs::last_write_time(o->host_path, ec));
        for (int i = 0; i < 4; i++) W64(info + 8 * i, ft);
        W32(info + 32, o->is_dir ? 0x10 : 0x80); iosb(iosb_p, 0, 40); break;
    }
    default:
        hle_log("[file] NtQueryInformationFile class %u not handled", cls);
        RET(c, STATUS_INVALID_PARAMETER, 5); return;
    }
    RET(c, STATUS_SUCCESS, 5);
}

static void NtSetInformationFile(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    uint32_t iosb_p = A(c, 1), info = A(c, 2), cls = A(c, 4);
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 5); return; }
    std::error_code ec;
    switch (cls) {
    case 14: o->pos = R64(info); break;                                      // position
    case 20: case 19:                                                        // end of file / allocation
        if (o->fp) { std::fflush(o->fp); fs::resize_file(o->host_path, R64(info), ec); }
        break;
    case 13:                                                                 // disposition (delete)
        if (R8(info) && !is_read_only(o->host_path)) {
            if (o->fp) { std::fclose(o->fp); o->fp = nullptr; }
            fs::remove(o->host_path, ec);
            if (log_enabled("file") || is_save_path(o->host_path))
                hle_log("[file] delete %s%s%s", o->host_path.c_str(), ec ? ": " : "", ec ? ec.message().c_str() : "");
        }
        break;
    case 4: break;                                                           // basic info: ignore times
    case 10: {                                                               // FileRenameInformation
        // { BOOLEAN ReplaceIfExists; HANDLE RootDirectory; STRING FileName }
        std::string target = ansi_string(info + 8);
        uint32_t root = R32(info + 4);
        if (root) { KObject* r = handle_get(root); if (r) target = r->guest_path + "\\" + target; }
        std::string dst = host_path(target);
        if (dst.empty() || is_read_only(o->host_path)) { RET(c, STATUS_ACCESS_DENIED, 5); return; }
        if (fs::exists(dst, ec) && !R8(info)) { RET(c, STATUS_OBJECT_NAME_COLLISION, 5); return; }
        bool reopen = o->fp != nullptr;
        if (o->fp) { std::fclose(o->fp); o->fp = nullptr; }
        fs::remove(dst, ec);
        fs::rename(o->host_path, dst, ec);
        if (ec) hle_log("[file] rename %s -> %s failed: %s", o->host_path.c_str(), dst.c_str(), ec.message().c_str());
        else { o->host_path = dst; o->guest_path = target; }
        if (reopen) o->fp = std::fopen(o->host_path.c_str(), "r+b");
        if (log_enabled("file")) hle_log("[file] renamed to %s", target.c_str());
        break;
    }
    default:
        hle_log("[file] NtSetInformationFile class %u not handled", cls);
    }
    iosb(iosb_p, 0, 0);
    RET(c, STATUS_SUCCESS, 5);
}

static void NtQueryFullAttributesFile(Cpu* c) {
    std::string path = host_path(object_name(A(c, 0)));
    std::error_code ec;
    bool found = !path.empty() && fs::exists(path, ec);
    if (log_enabled("file") || is_save_path(path)) hle_log("[file] attributes of %s: %s", path.c_str(), found ? "found" : "not found");
    if (!found) { RET(c, STATUS_OBJECT_NAME_NOT_FOUND, 2); return; }
    write_network_open_info(A(c, 1), path);
    RET(c, STATUS_SUCCESS, 2);
}

static bool wildcard(const char* p, const char* s) {
    if (!*p) return !*s;
    if (*p == '*') return wildcard(p + 1, s) || (*s && wildcard(p, s + 1));
    if (*s && (*p == '?' || std::tolower((unsigned char)*p) == std::tolower((unsigned char)*s)))
        return wildcard(p + 1, s + 1);
    return false;
}

static void NtQueryDirectoryFile(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    uint32_t iosb_p = A(c, 4), info = A(c, 5), len = A(c, 6), mask = A(c, 8), restart = A(c, 9);
    if (!o || !o->is_dir) { RET(c, STATUS_INVALID_HANDLE, 10); return; }
    if (restart || (o->dir_entries.empty() && o->dir_pos == 0)) {
        std::string m = mask ? ansi_string(mask) : "";
        if (m.empty()) m = "*";                  // FindFirstFile passes "*.*" as an empty mask
        o->dir_entries.clear();
        o->dir_pos = 0;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(o->host_path, ec)) {
            std::string n = e.path().filename().string();
            if (wildcard(m.c_str(), n.c_str())) o->dir_entries.push_back(n);
        }
        if (log_enabled("file") || is_save_path(o->host_path))
            hle_log("[file] list %s/%s: %zu entries", o->host_path.c_str(), m.c_str(), o->dir_entries.size());
    }
    if (o->dir_pos >= o->dir_entries.size()) { iosb(iosb_p, STATUS_NO_MORE_FILES, 0); RET(c, STATUS_NO_MORE_FILES, 10); return; }
    const std::string& name = o->dir_entries[o->dir_pos++];
    std::memset(gptr(info), 0, len);
    std::string full = o->host_path + "/" + name;
    std::error_code ec;
    bool dir = fs::is_directory(full, ec);
    uint64_t size = dir ? 0 : (uint64_t)fs::file_size(full, ec);
    uint64_t ft = to_filetime(fs::last_write_time(full, ec));
    for (int i = 0; i < 4; i++) W64(info + 8 + 8 * i, ft);
    W64(info + 0x28, size);
    W64(info + 0x30, (size + 0xfff) & ~0xfffull);
    W32(info + 0x38, dir ? 0x10 : 0x80);
    uint32_t n = (uint32_t)std::min<size_t>(name.size(), len > 0x40 ? len - 0x40 : 0);
    W32(info + 0x3c, n);
    std::memcpy(gptr(info + 0x40), name.data(), n);
    iosb(iosb_p, STATUS_SUCCESS, 0x40 + n);
    RET(c, STATUS_SUCCESS, 10);
}

static void NtQueryVolumeInformationFile(Cpu* c) {
    uint32_t iosb_p = A(c, 1), info = A(c, 2), cls = A(c, 4);
    if (cls == 3) {                                   // FileFsSizeInformation
        // Like a 750 MB Xbox cache partition: the game assumes free space < 4 GB.
        W64(info, 48000); W64(info + 8, 40960); W32(info + 16, 32); W32(info + 20, 512);
        iosb(iosb_p, 0, 24);
    } else if (cls == 1) {                            // FileFsVolumeInformation
        std::memset(gptr(info), 0, A(c, 3));
        W32(info + 8, 0x54454a33u);                   // serial number
        iosb(iosb_p, 0, 18);
    } else {
        hle_log("[file] volume information class %u", cls);
        std::memset(gptr(info), 0, A(c, 3));
        iosb(iosb_p, 0, A(c, 3));
    }
    RET(c, STATUS_SUCCESS, 5);
}

static void NtFlushBuffersFile(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (o && o->fp) std::fflush(o->fp);
    iosb(A(c, 1), 0, 0);
    RET(c, STATUS_SUCCESS, 2);
}
static void NtFsControlFile(Cpu* c) { iosb(A(c, 4), 0, 0); RET(c, STATUS_SUCCESS, 10); }
static void NtDeviceIoControlFile(Cpu* c) {
    uint32_t code = A(c, 5), in = A(c, 6);
    if (code == 0x4d014 && in) {          // IOCTL_SCSI_PASS_THROUGH_DIRECT
        uint32_t len = R32(in + 0x0c), buf = R32(in + 0x14), cdb = in + 0x1c;
        if (buf && len) std::memset(gptr(buf), 0, len);
        if (R8(cdb) == 0x5a && (R8(cdb + 2) & 0x3f) == 0x3e && buf && len >= 13) {
            // MODE SENSE, Xbox security page: an authenticated original disc.
            W8(buf + 10, 1);              // partition area
            W8(buf + 11, 1);              // CDF valid
            W8(buf + 12, 1);              // authenticated
        } else if (log_enabled("file")) {
            hle_log("[file] SCSI command %02x passed through as success", R8(cdb));
        }
    } else {
        hle_log("[file] NtDeviceIoControlFile code %08x", code);
    }
    iosb(A(c, 4), 0, 0);
    RET(c, STATUS_SUCCESS, 10);
}
static void NtReadFileScatter(Cpu* c) { hle_unimplemented("NtReadFileScatter", c); RET(c, STATUS_NOT_IMPLEMENTED, 8); }
static void NtWriteFileGather(Cpu* c) { hle_unimplemented("NtWriteFileGather", c); RET(c, STATUS_NOT_IMPLEMENTED, 8); }

static void IoCreateSymbolicLink(Cpu* c) {
    std::string link = lower(ansi_string(A(c, 0))), dev = ansi_string(A(c, 1));
    if (link.rfind("\\??\\", 0) == 0) link = link.substr(4);
    g_links[link.substr(0, 2)] = dev;
    if (log_enabled("file")) hle_log("[file] link %s -> %s", link.c_str(), dev.c_str());
    RET(c, STATUS_SUCCESS, 2);
}
static void IoDeleteSymbolicLink(Cpu* c) {
    std::string link = lower(ansi_string(A(c, 0)));
    if (link.rfind("\\??\\", 0) == 0) link = link.substr(4);
    g_links.erase(link.substr(0, 2));
    RET(c, STATUS_SUCCESS, 1);
}
static void NtOpenSymbolicLinkObject(Cpu* c) {
    std::string name = lower(object_name(A(c, 1)));
    if (name.rfind("\\??\\", 0) == 0) name = name.substr(4);
    auto* o = new KObject();
    o->guest_path = name.substr(0, 2);
    auto it = g_links.find(o->guest_path);
    if (it == g_links.end() && o->guest_path != "d:") { delete o; RET(c, STATUS_OBJECT_NAME_NOT_FOUND, 2); return; }
    W32(A(c, 0), handle_new(o));
    RET(c, STATUS_SUCCESS, 2);
}
static void NtQuerySymbolicLinkObject(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    std::string target = "\\Device\\CdRom0";
    if (o) { auto it = g_links.find(o->guest_path); if (it != g_links.end()) target = it->second; }
    uint32_t s = A(c, 1);
    uint32_t n = (uint32_t)std::min<size_t>(target.size(), R16(s + 2));
    std::memcpy(gptr(R32(s + 4)), target.data(), n);
    W16(s, (uint16_t)n);
    if (A(c, 2)) W32(A(c, 2), n);
    RET(c, STATUS_SUCCESS, 3);
}

// --- dispatcher objects (handles) -------------------------------------------------------------
static KObject* new_obj(ObjType t) { auto* o = new KObject(); o->type = t; return o; }

static void NtCreateEvent(Cpu* c) {
    KObject* o = new_obj(ObjType::Event);
    o->manual_reset = A(c, 2) == 0;          // NotificationEvent
    o->signaled = A(c, 3) != 0;
    W32(A(c, 0), handle_new(o));
    RET(c, STATUS_SUCCESS, 4);
}
static void NtSetEvent(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 2); return; }
    if (A(c, 1)) W32(A(c, 1), o->signaled);
    o->signaled = true;
    sched_wake_all();
    RET(c, STATUS_SUCCESS, 2);
}
static void NtClearEvent(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (o) o->signaled = false;
    RET(c, o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE, 1);
}
static void NtPulseEvent(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 2); return; }
    if (A(c, 1)) W32(A(c, 1), o->signaled);
    o->signaled = true;
    sched_wake_all();
    o->signaled = false;       // waiters already blocked are not woken reliably; acceptable for now
    RET(c, STATUS_SUCCESS, 2);
}
static void NtCreateSemaphore(Cpu* c) {
    KObject* o = new_obj(ObjType::Semaphore);
    o->count = (int32_t)A(c, 2);
    o->limit = (int32_t)A(c, 3);
    W32(A(c, 0), handle_new(o));
    RET(c, STATUS_SUCCESS, 4);
}
static void NtReleaseSemaphore(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 3); return; }
    if (A(c, 2)) W32(A(c, 2), (uint32_t)o->count);
    if (o->count + (int32_t)A(c, 1) > o->limit) { RET(c, STATUS_SEMAPHORE_LIMIT_EXCEEDED, 3); return; }
    o->count += (int32_t)A(c, 1);
    sched_wake_all();
    RET(c, STATUS_SUCCESS, 3);
}
static void NtCreateMutant(Cpu* c) {
    KObject* o = new_obj(ObjType::Mutant);
    if (A(c, 2)) { o->owner = current_kthread(); o->recursion = 1; }
    W32(A(c, 0), handle_new(o));
    RET(c, STATUS_SUCCESS, 3);
}
static void NtReleaseMutant(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 2); return; }
    if (o->owner != current_kthread()) { RET(c, STATUS_MUTANT_NOT_OWNED, 2); return; }
    if (A(c, 1)) W32(A(c, 1), (uint32_t)o->recursion);
    if (--o->recursion <= 0) { o->owner = 0; o->recursion = 0; sched_wake_all(); }
    RET(c, STATUS_SUCCESS, 2);
}
static void NtCreateTimer(Cpu* c) {
    KObject* o = new_obj(ObjType::Timer);
    o->manual_reset = A(c, 2) == 0;
    W32(A(c, 0), handle_new(o));
    RET(c, STATUS_SUCCESS, 3);
}
static void NtSetTimerEx(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 8); return; }
    if (A(c, 7)) W32(A(c, 7), o->signaled);
    int64_t rel = timeout_of(A(c, 1));
    o->due = now_100ns() + (rel < 0 ? 0 : rel);
    o->period_ms = (int32_t)A(c, 6);
    o->signaled = false;
    if (A(c, 2)) hle_log("[kernel] NtSetTimerEx with APC routine %08x (not delivered)", A(c, 2));
    RET(c, STATUS_SUCCESS, 8);
}
static void NtCancelTimer(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (o) { if (A(c, 1)) W32(A(c, 1), o->signaled); o->due = 0; }
    RET(c, o ? STATUS_SUCCESS : STATUS_INVALID_HANDLE, 2);
}

static void update_timer(KObject* o) {
    if (o->type == ObjType::Timer && o->due && now_100ns() >= o->due) {
        o->signaled = true;
        o->due = o->period_ms ? o->due + (int64_t)o->period_ms * 10000 : 0;
    }
}

static uint32_t current_thread_handle_obj(uint32_t h, KObject** out) {
    *out = handle_get(h);
    return *out ? 0 : STATUS_INVALID_HANDLE;
}

static uint32_t wait_handles(uint32_t count, const uint32_t* hs, bool all, uint32_t timeout_p, bool alertable = false) {
    current_thread()->blocked = "NtWait";
    if (log_enabled("sleep")) hle_log("[wait] t%u on %u handles (%x) timeout %lld us from %08x", current_thread()->id, count,
                                      hs[0], (long long)(timeout_of(timeout_p) / 10), R32(current_cpu()->esp));
    std::vector<KObject*> objs;
    for (uint32_t i = 0; i < count; i++) {
        KObject* o = handle_get(hs[i]);
        if (!o) { hle_log("[kernel] wait on unknown handle %x", hs[i]); return STATUS_INVALID_HANDLE; }
        objs.push_back(o);
    }
    uint32_t me = current_kthread();
    GuestThread* self = current_thread();
    int which = -1;
    bool apc = false;
    auto ready = [&]() {
        if (alertable && apc_pending(self)) { apc = true; return true; }
        for (auto* o : objs) update_timer(o);
        if (all) {
            for (auto* o : objs) if (!kobj_signaled(o, me)) return false;
            which = 0;
            return true;
        }
        for (size_t i = 0; i < objs.size(); i++)
            if (kobj_signaled(objs[i], me)) { which = (int)i; return true; }
        return false;
    };
    // Timers have no host thread signalling them: poll at their due time.
    int64_t t = timeout_of(timeout_p);
    bool ok = wait_until(ready, t);
    if (!ok) return STATUS_TIMEOUT;
    if (apc) return STATUS_USER_APC;
    if (all) for (auto* o : objs) kobj_acquire(o, me);
    else kobj_acquire(objs[which], me);
    return all ? 0 : (uint32_t)which;
}

static void NtWaitForSingleObject(Cpu* c) { uint32_t h = A(c, 0); RET(c, wait_handles(1, &h, false, A(c, 2)), 3); }
static void NtWaitForSingleObjectEx(Cpu* c) {     // (Handle, WaitMode, Alertable, Timeout)
    uint32_t h = A(c, 0);
    uint32_t st = wait_handles(1, &h, false, A(c, 3), A(c, 2) != 0);
    if (st == STATUS_USER_APC) deliver_apcs(c);
    RET(c, st, 4);
}
static void NtWaitForMultipleObjectsEx(Cpu* c) {  // (Count, Handles, WaitType, WaitMode, Alertable, Timeout)
    uint32_t n = A(c, 0);
    std::vector<uint32_t> hs(n);
    for (uint32_t i = 0; i < n; i++) hs[i] = R32(A(c, 1) + 4 * i);
    uint32_t st = wait_handles(n, hs.data(), A(c, 2) == 0, A(c, 5), A(c, 4) != 0);
    if (st == STATUS_USER_APC) deliver_apcs(c);
    RET(c, st, 6);
}
static void NtSignalAndWaitForSingleObjectEx(Cpu* c) {
    KObject* s = handle_get(A(c, 0));
    if (s) {
        if (s->type == ObjType::Event) s->signaled = true;
        else if (s->type == ObjType::Semaphore) s->count++;
        else if (s->type == ObjType::Mutant && --s->recursion <= 0) { s->owner = 0; s->recursion = 0; }
        sched_wake_all();
    }
    uint32_t h = A(c, 1);
    uint32_t st = wait_handles(1, &h, false, A(c, 4), A(c, 3) != 0);
    if (st == STATUS_USER_APC) deliver_apcs(c);
    RET(c, st, 5);
}
static void KeDelayExecutionThread(Cpu* c) {
    int64_t t = timeout_of(A(c, 2));
    if (log_enabled("sleep")) hle_log("[sleep] t%u %lld us from %08x", current_thread()->id, (long long)(t / 10), R32(c->esp));
    if (log_enabled("sleepbt") && current_thread()->id == 2) {
        static int n = 0;
        void log_backtrace(Cpu*, const char*);
        if (n++ % 20 == 0) log_backtrace(c, t >= 100000 ? "long sleep" : "short sleep");
    }
    if (A(c, 1)) {                       // alertable: queued APCs end the wait
        GuestThread* self = current_thread();
        bool apc = apc_pending(self) || wait_until([self] { return apc_pending(self); }, t <= 0 ? 0 : t);
        if (apc) {
            deliver_apcs(c);
            RET(c, STATUS_USER_APC, 3);
            return;
        }
        RET(c, STATUS_SUCCESS, 3);
        if (t <= 0) sched_yield();
        return;
    }
    RET(c, STATUS_SUCCESS, 3);
    extern volatile uint32_t g_profile_fn;
    g_profile_fn = 0xfffffffeu;          // profiler: sleeping
    if (t <= 0) { sched_yield(); park_if_suspended(); return; }
    auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(t / 10);
    without_gil([until] { std::this_thread::sleep_until(until); });
}
static void NtYieldExecution(Cpu* c) {
    RET(c, STATUS_SUCCESS, 0);
    sched_yield();
    park_if_suspended();
}
static void NtClose(Cpu* c) { RET(c, handle_close(A(c, 0)) ? STATUS_SUCCESS : STATUS_INVALID_HANDLE, 1); }
static void NtDuplicateObject(Cpu* c) {
    KObject* o = handle_get(A(c, 0));
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 3); return; }
    // Share the object: register it under a second handle (closing one must not free it).
    auto* alias = new KObject(*o);
    alias->fp = o->fp ? std::fopen(o->host_path.c_str(), "rb") : nullptr;
    W32(A(c, 1), handle_new(o->type == ObjType::Thread ? o : alias));
    if (A(c, 2) & 1) handle_close(A(c, 0));        // DUPLICATE_CLOSE_SOURCE
    RET(c, STATUS_SUCCESS, 3);
}

// --- threads ---------------------------------------------------------------------------------
static void PsCreateSystemThreadEx(Cpu* c) {
    uint32_t tid = 0;
    uint32_t h = create_thread(A(c, 9), A(c, 5), A(c, 6), A(c, 2), A(c, 3), A(c, 7) != 0, &tid);
    W32(A(c, 0), h);
    if (A(c, 4)) W32(A(c, 4), tid);
    RET(c, STATUS_SUCCESS, 10);
}
static void PsTerminateSystemThread(Cpu* c) { exit_thread(A(c, 0)); }
int suspend_thread(uint32_t handle, int delta);
void park_if_suspended();
static void NtSuspendThread(Cpu* c) {
    int prev = suspend_thread(A(c, 0), +1);
    if (prev < 0) { RET(c, STATUS_INVALID_HANDLE, 2); return; }
    if (A(c, 1)) W32(A(c, 1), (uint32_t)prev);
    RET(c, STATUS_SUCCESS, 2);
    park_if_suspended();          // a thread may suspend itself (or must stop for another)
}
static void NtResumeThread(Cpu* c) {
    int prev = suspend_thread(A(c, 0), -1);
    if (prev < 0) { RET(c, STATUS_INVALID_HANDLE, 2); return; }
    if (A(c, 1)) W32(A(c, 1), (uint32_t)prev);
    RET(c, STATUS_SUCCESS, 2);
}
// Base priority kept in our KTHREAD block (+0x200): no scheduling effect yet, but
// callers read back what they set (CRI waits on its own priority changes).
static void KeSetBasePriorityThread(Cpu* c) {
    uint32_t t = A(c, 0);
    uint32_t old = R32(t + 0x200);
    W32(t + 0x200, A(c, 1));
    if (log_enabled("thread")) hle_log("[thread] t%u priority %d", R32(t + 0x130), (int32_t)A(c, 1));
    RET(c, old, 2);
}
static void KeQueryBasePriorityThread(Cpu* c) { RET(c, R32(A(c, 0) + 0x200), 1); }
static void KeSetDisableBoostThread(Cpu* c) { RET(c, 0, 2); }
// NtQueueApcThread(ThreadHandle, ApcRoutine, ApcRoutineContext, ApcStatusBlock, ApcReserved)
static void NtQueueApcThread(Cpu* c) {
    GuestThread* t = nullptr;
    if (A(c, 0) == 0xfffffffe) t = current_thread();
    else if (KObject* o = handle_get(A(c, 0))) t = o->thread;
    if (!t) { RET(c, STATUS_INVALID_HANDLE, 5); return; }
    queue_apc(t, A(c, 1), A(c, 2), A(c, 3), A(c, 4));
    RET(c, STATUS_SUCCESS, 5);
}

static uint32_t g_object_types[6];      // Ex{Event,Semaphore,Mutant,Timer}ObjectType, PsThreadObjectType, IoFileObjectType

static void ObReferenceObjectByHandle(Cpu* c) {
    uint32_t h = A(c, 0), out = A(c, 2);
    if (h == 0xfffffffe) { W32(out, current_kthread()); RET(c, STATUS_SUCCESS, 3); return; }
    KObject* o = handle_get(h);
    if (!o) { RET(c, STATUS_INVALID_HANDLE, 3); return; }
    W32(out, o->thread ? o->thread->kthread : 0x8f0f0000u + h);
    RET(c, STATUS_SUCCESS, 3);
}
static void ObfDereferenceObject(Cpu* c) { c->esp += 4; }       // fastcall (ecx)
static void ObOpenObjectByName(Cpu* c) {
    hle_log("[kernel] ObOpenObjectByName %s", object_name(A(c, 0)).c_str());
    RET(c, STATUS_OBJECT_NAME_NOT_FOUND, 4);
}

// --- critical sections and guest dispatcher objects ---------------------------------------------
// RTL_CRITICAL_SECTION: +0x00 KEVENT, +0x10 LockCount, +0x14 RecursionCount, +0x18 OwningThread
static void RtlInitializeCriticalSection(Cpu* c) {
    uint32_t cs = A(c, 0);
    std::memset(gptr(cs), 0, 0x1c);
    W8(cs, 1); W8(cs + 2, 4);                 // SynchronizationEvent header
    W32(cs + 0x10, 0xffffffffu);
    RET(c, 0, 1);
}
static void RtlEnterCriticalSection(Cpu* c) {
    uint32_t cs = A(c, 0), me = current_kthread();
    note_hle_call(c, "RtlEnterCriticalSection");
    if (R32(cs + 0x18) != me && R32(cs + 0x18) != 0) current_thread()->blocked = "critical section";
    if (R32(cs + 0x18) == me) {
        W32(cs + 0x14, R32(cs + 0x14) + 1);
        W32(cs + 0x10, R32(cs + 0x10) + 1);
    } else {
        if (R32(cs + 0x18) && log_enabled("cs"))
            hle_log("[cs] t%u waits for %08x owned by thread %u (kthread %08x) from %08x", current_thread()->id, cs,
                    R32(R32(cs + 0x18) + 0x130), R32(cs + 0x18), R32(c->esp));
        wait_until([&] { return R32(cs + 0x18) == 0; }, -1);
        W32(cs + 0x18, me);
        W32(cs + 0x14, 1);
        W32(cs + 0x10, R32(cs + 0x10) + 1);
    }
    RET(c, 0, 1);
}
static void RtlLeaveCriticalSection(Cpu* c) {
    uint32_t cs = A(c, 0);
    W32(cs + 0x10, R32(cs + 0x10) - 1);
    uint32_t rec = R32(cs + 0x14) - 1;
    W32(cs + 0x14, rec);
    if (rec == 0) { W32(cs + 0x18, 0); sched_wake_all(); }
    RET(c, 0, 1);
}

// Guest DISPATCHER_HEADER: u8 Type, u8 Absolute, u8 Size, u8 Inserted, s32 SignalState, LIST_ENTRY
static void KeSetEvent(Cpu* c) {
    uint32_t ev = A(c, 0);
    uint32_t prev = R32(ev + 4);
    W32(ev + 4, 1);
    sched_wake_all();
    RET(c, prev, 3);
}
static bool guest_obj_signaled(uint32_t o) {
    uint8_t type = R8(o);
    if (type == 8 || type == 9) {               // KTIMER: DueTime at +0x10 (absolute interrupt time)
        int64_t due = R64s(o + 0x10);
        if (due && now_100ns() - g_boot_100ns >= due) W32(o + 4, 1);
    }
    return (int32_t)R32(o + 4) > 0;
}
static void KeWaitForSingleObject(Cpu* c) {
    uint32_t o = A(c, 0);
    GuestThread* self = current_thread();
    self->blocked = "KeWait";
    bool alertable = A(c, 3) != 0, apc = false;
    bool ok = wait_until([&] {
        if (alertable && apc_pending(self)) { apc = true; return true; }
        return guest_obj_signaled(o);
    }, timeout_of(A(c, 4)));
    if (ok && apc) {
        deliver_apcs(c);
        RET(c, STATUS_USER_APC, 5);
        return;
    }
    if (ok) {
        uint8_t type = R8(o);
        if (type == 1 || type == 9) W32(o + 4, 0);          // synchronization event / timer
        else if (type == 5) W32(o + 4, R32(o + 4) - 1);     // semaphore
    }
    RET(c, ok ? STATUS_SUCCESS : STATUS_TIMEOUT, 5);
}
static void KeInitializeTimerEx(Cpu* c) {
    uint32_t t = A(c, 0);
    std::memset(gptr(t), 0, 0x28);
    W8(t, (uint8_t)(8 + A(c, 1)));             // NotificationTimer / SynchronizationTimer
    W8(t + 2, 10);
    RET(c, 0, 2);
}
static void KeSetTimer(Cpu* c) {          // (Timer, DueTime (by value, 8 bytes), Dpc)
    uint32_t t = A(c, 0);
    int64_t due = (int64_t)(((uint64_t)A(c, 2) << 32) | A(c, 1));
    int64_t now = now_100ns() - g_boot_100ns;
    W64s(t + 0x10, due < 0 ? now - due : now + 1);
    uint32_t was = R8(t + 3);
    W8(t + 3, 1);
    W32(t + 4, 0);
    if (A(c, 3)) {
        hle_log("[kernel] KeSetTimer due %lld with DPC %08x routine %08x ctx %08x (not delivered)", (long long)due,
                A(c, 3), R32(A(c, 3) + 0x0c), R32(A(c, 3) + 0x10));
        void log_backtrace(Cpu*, const char*);
        log_backtrace(c, "KeSetTimer");
    }
    RET(c, was, 4);
}
static void KeCancelTimer(Cpu* c) {
    uint32_t t = A(c, 0);
    uint32_t was = R8(t + 3);
    W8(t + 3, 0);
    W64(t + 0x10, 0);
    RET(c, was, 1);
}
static void KeInitializeDpc(Cpu* c) {
    uint32_t d = A(c, 0);
    std::memset(gptr(d), 0, 0x1c);
    W16(d, 19);
    W32(d + 0x0c, A(c, 1));
    W32(d + 0x10, A(c, 2));
    RET(c, 0, 3);
}
static void KeInsertQueueDpc(Cpu* c) {
    uint32_t d = A(c, 0), a1 = A(c, 1), a2 = A(c, 2);
    c->esp += 16;
    c->eax = 1;
    // Run the DPC right away: DeferredRoutine(Dpc, DeferredContext, Arg1, Arg2), stdcall.
    uint32_t routine = R32(d + 0x0c), ctx = R32(d + 0x10);
    uint32_t saved_eax = c->eax;
    c->esp -= 16;
    W32(c->esp, d); W32(c->esp + 4, ctx); W32(c->esp + 8, a1); W32(c->esp + 12, a2);
    c->esp -= 4; W32(c->esp, 0);
    call_indirect(c, routine);
    c->eax = saved_eax;
}
static void KeRemoveQueueDpc(Cpu* c) { RET(c, 0, 1); }
static void KeInitializeInterrupt(Cpu* c) { RET(c, 0, 7); }
static void KeConnectInterrupt(Cpu* c) { RET(c, 1, 1); }
static void KeDisconnectInterrupt(Cpu* c) { RET(c, 1, 1); }
static void KeSynchronizeExecution(Cpu* c) {
    uint32_t routine = A(c, 1), ctx = A(c, 2);
    c->esp += 16;
    c->esp -= 4; W32(c->esp, ctx);
    c->esp -= 4; W32(c->esp, 0);
    call_indirect(c, routine);
}
static void KeStallExecutionProcessor(Cpu* c) { RET(c, 0, 1); }
static void KfRaiseIrql(Cpu* c) {           // fastcall: cl = new irql, returns old
    uint32_t pcr = c->fs_base;
    uint32_t old = R8(pcr + 0x24);
    W8(pcr + 0x24, (uint8_t)c->ecx);
    c->eax = old;
    c->esp += 4;
}
static void KfLowerIrql(Cpu* c) { W8(c->fs_base + 0x24, (uint8_t)c->ecx); c->esp += 4; }
static void KeRaiseIrqlToDpcLevel(Cpu* c) {
    uint32_t old = R8(c->fs_base + 0x24);
    W8(c->fs_base + 0x24, 2);
    RET(c, old, 0);
}
static void KeSaveFloatingPointState(Cpu* c) { RET(c, STATUS_SUCCESS, 1); }
static void KeRestoreFloatingPointState(Cpu* c) { RET(c, STATUS_SUCCESS, 1); }
static void KeBugCheck(Cpu* c) { hle_log("[kernel] KeBugCheck %08x", A(c, 0)); fault("KeBugCheck", A(c, 0)); }
static void IofCompleteRequest(Cpu* c) { c->esp += 4; }

// --- memory --------------------------------------------------------------------------------
// Virtual memory: reservations are remembered so that commits (and decommits)
// at addresses inside them stay there; the heap grows its segments that way.
static std::map<uint32_t, uint32_t> g_vm_regions;     // base -> size
static std::mutex g_vm_m;

static bool vm_region_of(uint32_t addr, uint32_t* base, uint32_t* size) {
    std::lock_guard<std::mutex> lk(g_vm_m);
    auto it = g_vm_regions.upper_bound(addr);
    if (it == g_vm_regions.begin()) return false;
    --it;
    if (addr >= it->first + it->second) return false;
    *base = it->first;
    *size = it->second;
    return true;
}

// NtAllocateVirtualMemory(*BaseAddress, ZeroBits, *RegionSize, AllocationType, Protect)
static void NtAllocateVirtualMemory(Cpu* c) {
    uint32_t base_p = A(c, 0), size_p = A(c, 2);
    uint32_t want = R32(base_p), size = R32(size_p);
    uint32_t rbase, rsize;
    if (want && vm_region_of(want, &rbase, &rsize)) {         // commit inside an earlier reservation
        uint32_t lo = want & ~0xfffu, hi = (want + size + 0xfff) & ~0xfffu;
        if (hi > rbase + rsize) hi = rbase + rsize;
        W32(base_p, lo);
        W32(size_p, hi - lo);
        RET(c, STATUS_SUCCESS, 5);
        return;
    }
    size = (size + 0xfff) & ~0xfffu;
    uint32_t a = guest_alloc(size, 0x10000);
    if (!a) { RET(c, STATUS_NO_MEMORY, 5); return; }
    std::memset(gptr(a), 0, size);
    if (want) hle_log("[mem] NtAllocateVirtualMemory wanted %08x (not reserved) got %08x", want, a);
    {
        std::lock_guard<std::mutex> lk(g_vm_m);
        g_vm_regions[a] = size;
    }
    W32(base_p, a);
    W32(size_p, size);
    RET(c, STATUS_SUCCESS, 5);
}
// NtFreeVirtualMemory(*BaseAddress, *RegionSize, FreeType): MEM_DECOMMIT 0x4000, MEM_RELEASE 0x8000
static void NtFreeVirtualMemory(Cpu* c) {
    uint32_t base_p = A(c, 0), size_p = A(c, 1), type = A(c, 2);
    uint32_t addr = R32(base_p), rbase, rsize;
    if (!vm_region_of(addr, &rbase, &rsize)) {
        if (type & 0x8000) guest_free(addr);
        RET(c, STATUS_SUCCESS, 3);
        return;
    }
    if (type & 0x8000) {                                       // release the whole reservation
        {
            std::lock_guard<std::mutex> lk(g_vm_m);
            g_vm_regions.erase(rbase);
        }
        guest_free(rbase);
        W32(base_p, rbase);
        if (size_p) W32(size_p, rsize);
    } else if (type & 0x4000) {                                // decommit: pages read as zero when committed again
        uint32_t lo = addr & ~0xfffu, n = size_p ? R32(size_p) : 0;
        uint32_t hi = n ? (addr + n + 0xfff) & ~0xfffu : rbase + rsize;
        if (hi > rbase + rsize) hi = rbase + rsize;
        std::memset(gptr(lo), 0, hi - lo);
        W32(base_p, lo);
        if (size_p) W32(size_p, hi - lo);
    }
    RET(c, STATUS_SUCCESS, 3);
}
// NtQueryVirtualMemory(BaseAddress, MEMORY_BASIC_INFORMATION*)
static void NtQueryVirtualMemory(Cpu* c) {
    uint32_t addr = A(c, 0), info = A(c, 1), rbase, rsize;
    uint32_t page = addr & ~0xfffu;
    if (vm_region_of(addr, &rbase, &rsize)) {
        W32(info, page); W32(info + 4, rbase); W32(info + 8, 4);
        W32(info + 12, rbase + rsize - page); W32(info + 16, 0x1000); W32(info + 20, 4); W32(info + 24, 0x20000);
    } else {
        uint32_t size = guest_alloc_size(page);
        W32(info, page); W32(info + 4, page); W32(info + 8, 4);
        W32(info + 12, size ? size : 0x1000); W32(info + 16, 0x1000); W32(info + 20, 4); W32(info + 24, 0x20000);
    }
    RET(c, STATUS_SUCCESS, 2);
}
static void MmAllocateContiguousMemory(Cpu* c) { RET(c, guest_alloc_contiguous(A(c, 0), 0x1000), 1); }
static void MmAllocateContiguousMemoryEx(Cpu* c) {
    uint32_t align = A(c, 3) ? A(c, 3) : 0x1000;
    RET(c, guest_alloc_contiguous(A(c, 0), align), 5);
}
static void MmFreeContiguousMemory(Cpu* c) { guest_free(A(c, 0)); RET(c, 0, 1); }
static void MmAllocateSystemMemory(Cpu* c) { RET(c, guest_alloc((A(c, 0) + 0xfff) & ~0xfffu, 0x1000), 2); }
static void MmFreeSystemMemory(Cpu* c) { guest_free(A(c, 0)); RET(c, A(c, 1), 2); }
static void MmQueryAllocationSize(Cpu* c) { RET(c, guest_alloc_size(A(c, 0)), 1); }
static void MmSetAddressProtect(Cpu* c) { RET(c, 0, 3); }
static void MmQueryAddressProtect(Cpu* c) { RET(c, 4, 1); }        // PAGE_READWRITE
static void MmPersistContiguousMemory(Cpu* c) { RET(c, 0, 3); }
static void MmGetPhysicalAddress(Cpu* c) {
    uint32_t a = A(c, 0);
    RET(c, a >= kContigBase && a < kContigEnd ? a - kContigBase : a & 0x0fffffff, 1);
}
static void MmClaimGpuInstanceMemory(Cpu* c) {
    if (A(c, 1)) W32(A(c, 1), 0);
    RET(c, kContigBase + 0x100000, 2);       // instance memory sits at the top on hardware; unused here
}
static void MmLockUnlockBufferPages(Cpu* c) { RET(c, 0, 3); }
static void MmLockUnlockPhysicalPage(Cpu* c) { RET(c, 0, 2); }
static void ExAllocatePool(Cpu* c) { RET(c, guest_alloc(A(c, 0)), 1); }
static void ExAllocatePoolWithTag(Cpu* c) { RET(c, guest_alloc(A(c, 0)), 2); }
static void ExFreePool(Cpu* c) { guest_free(A(c, 0)); RET(c, 0, 1); }
static void ExQueryPoolBlockSize(Cpu* c) { RET(c, guest_alloc_size(A(c, 0)), 1); }

// --- time ----------------------------------------------------------------------------------------
static int64_t system_time_100ns() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count() / 100 + 116444736000000000ll;
}
// Minutes to add to the PC's local time to get UTC, as the Xbox stores its time zone.
static int32_t utc_bias_minutes() {
    std::time_t now = std::time(nullptr);
    std::tm utc{}, local{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
    localtime_s(&local, &now);
    return (int32_t)((_mkgmtime(&utc) - _mkgmtime(&local)) / 60);
#else
    gmtime_r(&now, &utc);
    localtime_r(&now, &local);
    return (int32_t)((timegm(&utc) - timegm(&local)) / 60);
#endif
}
static void KeQuerySystemTime(Cpu* c) { W64s(A(c, 0), system_time_100ns()); RET(c, 0, 1); }
static void KeQueryInterruptTime(Cpu* c) {
    uint64_t t = (uint64_t)(now_100ns() - g_boot_100ns);
    c->edx = (uint32_t)(t >> 32);
    RET(c, (uint32_t)t, 0);
}
static void RtlTimeToTimeFields(Cpu* c) {
    int64_t t = R64s(A(c, 0));
    uint32_t f = A(c, 1);
    int64_t secs = t / 10000000 - 11644473600ll;
    int ms = (int)((t / 10000) % 1000);
    std::time_t tt = (std::time_t)secs;
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &tt);
#else
    gmtime_r(&tt, &tmv);
#endif
    W16(f, (uint16_t)(tmv.tm_year + 1900)); W16(f + 2, (uint16_t)(tmv.tm_mon + 1)); W16(f + 4, (uint16_t)tmv.tm_mday);
    W16(f + 6, (uint16_t)tmv.tm_hour); W16(f + 8, (uint16_t)tmv.tm_min); W16(f + 10, (uint16_t)tmv.tm_sec);
    W16(f + 12, (uint16_t)ms); W16(f + 14, (uint16_t)tmv.tm_wday);
    RET(c, 0, 2);
}
static void RtlTimeFieldsToTime(Cpu* c) {
    uint32_t f = A(c, 0);
    std::tm tmv{};
    tmv.tm_year = R16(f) - 1900; tmv.tm_mon = R16(f + 2) - 1; tmv.tm_mday = R16(f + 4);
    tmv.tm_hour = R16(f + 6); tmv.tm_min = R16(f + 8); tmv.tm_sec = R16(f + 10);
#ifdef _WIN32
    int64_t secs = _mkgmtime(&tmv);
#else
    int64_t secs = timegm(&tmv);
#endif
    W64s(A(c, 1), (secs + 11644473600ll) * 10000000 + (int64_t)R16(f + 12) * 10000);
    RET(c, 1, 2);
}

// --- strings and memory utilities ------------------------------------------------------------------
static void RtlInitAnsiString(Cpu* c) {
    uint32_t d = A(c, 0), s = A(c, 1);
    uint16_t n = s ? (uint16_t)guest_cstr(s, 0xfffe).size() : 0;
    W16(d, n); W16(d + 2, s ? n + 1 : 0); W32(d + 4, s);
    RET(c, 0, 2);
}
static void RtlInitUnicodeString(Cpu* c) {
    uint32_t d = A(c, 0), s = A(c, 1);
    uint16_t n = 0;
    if (s) while (R16(s + n)) n += 2;
    W16(d, n); W16(d + 2, s ? n + 2 : 0); W32(d + 4, s);
    RET(c, 0, 2);
}
static int compare_strings(uint32_t a, uint32_t b, bool ci) {
    std::string x = ansi_string(a), y = ansi_string(b);
    if (ci) {                            // NT compares in upper case; it decides how '_' and '\' sort
        for (auto& ch : x) ch = (char)std::toupper((unsigned char)ch);
        for (auto& ch : y) ch = (char)std::toupper((unsigned char)ch);
    }
    return x.compare(y);
}
static void RtlCompareString(Cpu* c) { RET(c, (uint32_t)compare_strings(A(c, 0), A(c, 1), A(c, 2) & 0xff), 3); }
static void RtlEqualString(Cpu* c) { RET(c, compare_strings(A(c, 0), A(c, 1), A(c, 2) & 0xff) == 0, 3); }
static void RtlFreeAnsiString(Cpu* c) {
    uint32_t s = A(c, 0);
    if (R32(s + 4)) guest_free(R32(s + 4));
    W32(s + 4, 0); W16(s, 0); W16(s + 2, 0);
    RET(c, 0, 1);
}
static void RtlUnicodeStringToAnsiString(Cpu* c) {
    uint32_t d = A(c, 0), s = A(c, 1);
    uint16_t n = R16(s) / 2;
    if (A(c, 2) & 0xff) { W32(d + 4, guest_alloc(n + 1)); W16(d + 2, n + 1); }
    uint32_t buf = R32(d + 4), src = R32(s + 4);
    uint16_t m = std::min<uint16_t>(n, R16(d + 2) ? R16(d + 2) - 1 : 0);
    for (uint16_t i = 0; i < m; i++) W8(buf + i, (uint8_t)R16(src + 2 * i));
    W8(buf + m, 0);
    W16(d, m);
    RET(c, STATUS_SUCCESS, 3);
}
static uint32_t dos_error(uint32_t s) {
    uint32_t e;
    switch (s) {
    case 0: e = 0; break;
    case STATUS_OBJECT_NAME_NOT_FOUND: e = 2; break;      // ERROR_FILE_NOT_FOUND
    case STATUS_OBJECT_PATH_NOT_FOUND: e = 3; break;
    case STATUS_ACCESS_DENIED: e = 5; break;
    case STATUS_INVALID_HANDLE: e = 6; break;
    case STATUS_NO_MEMORY: e = 8; break;
    case STATUS_END_OF_FILE: e = 38; break;               // ERROR_HANDLE_EOF
    case STATUS_NO_MORE_FILES: e = 18; break;
    case STATUS_OBJECT_NAME_COLLISION: e = 183; break;    // ERROR_ALREADY_EXISTS
    case STATUS_TIMEOUT: e = 1460; break;
    case STATUS_PENDING: e = 997; break;
    default: e = 31; break;                               // ERROR_GEN_FAILURE
    }
    return e;
}
static void RtlNtStatusToDosError(Cpu* c) { RET(c, dos_error(A(c, 0)), 1); }
static void RtlFillMemoryUlong(Cpu* c) {
    uint32_t d = A(c, 0), n = A(c, 1), v = A(c, 2);
    for (uint32_t i = 0; i + 4 <= n; i += 4) W32(d + i, v);
    RET(c, 0, 3);
}
static void RtlCompareMemory(Cpu* c) {
    uint32_t a = A(c, 0), b = A(c, 1), n = A(c, 2), i = 0;
    while (i < n && R8(a + i) == R8(b + i)) i++;
    RET(c, i, 3);
}
static void RtlCompareMemoryUlong(Cpu* c) {
    uint32_t a = A(c, 0), n = A(c, 1), v = A(c, 2), i = 0;
    while (i + 4 <= n && R32(a + i) == v) i += 4;
    RET(c, i, 3);
}
static void RtlRaiseException(Cpu* c) {
    hle_log("[kernel] RtlRaiseException code %08x", R32(A(c, 0)));
    fault("RtlRaiseException", R32(A(c, 0)));
}
static void RtlUnwind(Cpu* c) {
    // Pop SEH frames down to TargetFrame (handlers are not run: the game does not rely on them).
    uint32_t target = A(c, 0);
    uint32_t pcr = c->fs_base;
    uint32_t f = R32(pcr);
    while (f != 0xffffffffu && f != target && f) f = R32(f);
    W32(pcr, target ? target : f);
    RET(c, 0, 4);
}

// DbgPrint(format, ...) cdecl: format the guest arguments (%d %i %u %x %X %c %s %p).
static std::string guest_format(uint32_t fmt, uint32_t args) {
    std::string f = guest_cstr(fmt), out;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') { out += f[i]; continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < f.size() && std::strchr("-+ #0123456789.lhI", f[j])) spec += f[j++];
        if (j >= f.size()) break;
        char conv = f[j];
        char buf[512];
        if (conv == '%') { out += '%'; i = j; continue; }
        if (conv == 's') { std::snprintf(buf, sizeof buf, (spec + "s").c_str(), guest_cstr(R32(args)).c_str()); args += 4; }
        else if (conv == 'f' || conv == 'g' || conv == 'e') {
            double d; std::memcpy(&d, gptr(args), 8); args += 8;
            std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
        } else {
            std::string s2;
            for (char ch : spec) if (ch != 'l' && ch != 'h' && ch != 'I') s2 += ch;
            std::snprintf(buf, sizeof buf, (s2 + conv).c_str(), R32(args)); args += 4;
        }
        out += buf;
        i = j;
    }
    return out;
}
std::string guest_printf(uint32_t fmt, uint32_t args) { return guest_format(fmt, args); }

static void DbgPrint(Cpu* c) {
    std::string s = guest_format(A(c, 0), c->esp + 8);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    hle_log("[dbg] %s", s.c_str());
    RET(c, 0, 0);
}
static void DbgBreakPoint(Cpu* c) { hle_log("[kernel] DbgBreakPoint"); RET(c, 0, 0); }

// --- Xbox specifics ------------------------------------------------------------------------------------
static void ExQueryNonVolatileSetting(Cpu* c) {
    uint32_t idx = A(c, 0), type_p = A(c, 1), val = A(c, 2), len = A(c, 3), res = A(c, 4);
    uint32_t v = 0;
    if (idx == 0xffff) {                 // the whole EEPROM: zeros, so nothing reads leftover memory
        uint32_t n = std::min(len, 256u);
        if (val) std::memset(gptr(val), 0, n);
        if (type_p) W32(type_p, 3);      // REG_BINARY
        if (res) W32(res, n);
        RET(c, STATUS_SUCCESS, 5);
        return;
    }
    if (idx == 0xff) {                   // the user settings block: GetLocalTime reads the time zone here
        uint32_t n = std::min(len, 256u);
        if (val) std::memset(gptr(val), 0, n);
        if (val && n >= 0x5c) {
            int32_t bias = utc_bias_minutes();
            W32(val + 0x04, (uint32_t)bias);
            W32(val + 0x58, 2);          // daylight saving off: the bias already includes it
            if (log_enabled("kernel")) hle_log("[kernel] time zone bias %d minutes", bias);
        }
        if (type_p) W32(type_p, 3);      // REG_BINARY
        if (res) W32(res, n);
        RET(c, STATUS_SUCCESS, 5);
        return;
    }
    switch (idx) {
    case 0x7: v = 1; break;              // language: English
    case 0x8: v = 0; break;              // video flags
    case 0x9: v = 0; break;              // audio flags: stereo
    case 0xa: case 0xb: v = 0; break;    // parental control
    case 0x103: v = 0x00400100; break;   // factory AV region: NTSC-M
    case 0x104: v = 1; break;            // game region: North America
    default: if (log_enabled("kernel")) hle_log("[kernel] non-volatile setting %x", idx);
    }
    if (type_p) W32(type_p, 4);          // REG_DWORD
    if (val && len >= 4) W32(val, v);
    if (res) W32(res, 4);
    RET(c, STATUS_SUCCESS, 5);
}
void log_backtrace(Cpu* c, const char* why);
static void HalReturnToFirmware(Cpu* c) { hle_log("[kernel] HalReturnToFirmware(%u) from %08x: exiting", A(c, 0), R32(c->esp)); log_backtrace(c, "reboot"); std::fflush(g_log); std::_Exit(0); }
static void HalInitiateShutdown(Cpu* c) { hle_log("[kernel] HalInitiateShutdown"); std::_Exit(0); (void)c; }
static void HalReadWritePCISpace(Cpu* c) {
    if (!A(c, 5)) std::memset(gptr(A(c, 3)), 0, A(c, 4));
    RET(c, 0, 6);
}
static void HalGetInterruptVector(Cpu* c) { if (A(c, 1)) W8(A(c, 1), 0); RET(c, A(c, 0) + 0x30, 2); }
static void HalRegisterShutdownNotification(Cpu* c) { RET(c, 0, 2); }
static uint32_t g_av_saved = 0;
static void AvGetSavedDataAddress(Cpu* c) { RET(c, g_av_saved, 0); }
static void AvSetSavedDataAddress(Cpu* c) { g_av_saved = A(c, 0); RET(c, 0, 1); }
static void AvSendTVEncoderOption(Cpu* c) {
    if (A(c, 3)) W32(A(c, 3), A(c, 1) == 6 ? 0x00400100u : 0);   // AV_QUERY_AV_CAPABILITIES -> NTSC
    RET(c, 0, 4);
}
static void AvSetDisplayMode(Cpu* c) { RET(c, 0, 6); }
static void XeLoadSection(Cpu* c) {
    uint32_t s = A(c, 0);
    W32(s + 0x0c, R32(s + 0x0c) + 1);     // SectionReferenceCount (sections are always resident)
    RET(c, STATUS_SUCCESS, 1);
}
static void XeUnloadSection(Cpu* c) {
    uint32_t s = A(c, 0);
    if (R32(s + 0x0c)) W32(s + 0x0c, R32(s + 0x0c) - 1);
    RET(c, STATUS_SUCCESS, 1);
}
static void FscSetCacheSize(Cpu* c) { RET(c, STATUS_SUCCESS, 1); }
static void FscGetCacheSize(Cpu* c) { RET(c, 0x200, 0); }
static void NtCreateIoCompletion(Cpu* c) {
    W32(A(c, 0), handle_new(new_obj(ObjType::IoCompletion)));
    RET(c, STATUS_SUCCESS, 4);
}
static void NtSetIoCompletion(Cpu* c) { RET(c, STATUS_SUCCESS, 5); }
static void NtRemoveIoCompletion(Cpu* c) { RET(c, STATUS_TIMEOUT, 5); }
// NtUserIoApcDispatcher(ApcContext = completion routine, IoStatusBlock = &OVERLAPPED, Reserved):
// XAPI's I/O completion APC calls CompletionRoutine(dwErrorCode, dwBytes, lpOverlapped).
static void NtUserIoApcDispatcher(Cpu* c) {
    uint32_t routine = A(c, 0), iosb_p = A(c, 1);
    uint32_t status = R32(iosb_p);
    uint32_t err = status >= 0xc0000000u ? dos_error(status) : 0;
    guest_call(c, routine, {err, R32(iosb_p + 4), iosb_p});
    RET(c, 0, 3);
}

// --- SHA-1 / HMAC (save game signatures) -------------------------------------------------------------
struct Sha1 {
    uint32_t h[5]; uint64_t len; uint8_t buf[64]; uint32_t n;
    void init() { h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0; len = 0; n = 0; }
    static uint32_t rol(uint32_t x, int k) { return (x << k) | (x >> (32 - k)); }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | p[4 * i + 1] << 16 | p[4 * i + 2] << 8 | p[4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c2 = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c2) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c2 ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c2) | (b & d) | (c2 & d); k = 0x8F1BBCDC; }
            else { f = b ^ c2 ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c2; c2 = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c2; h[3] += d; h[4] += e;
    }
    void update(const uint8_t* p, size_t k) {
        len += k;
        while (k--) { buf[n++] = *p++; if (n == 64) { block(buf); n = 0; } }
    }
    void final(uint8_t* out) {
        uint64_t bits = len * 8;
        uint8_t pad = 0x80; update(&pad, 1);
        uint8_t z = 0; while (n != 56) update(&z, 1);
        uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
        update(lb, 8);
        for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) out[4 * i + j] = (uint8_t)(h[i] >> (24 - 8 * j));
    }
};
static std::string hex20(uint32_t a) {
    std::string r; char b[3];
    for (int i = 0; i < 20; i++) { std::snprintf(b, sizeof b, "%02x", R8(a + i)); r += b; }
    return r;
}
static_assert(sizeof(Sha1) <= 0x74, "SHA context must fit the guest's XcSHA buffer");
static void XcSHAInit(Cpu* c) { Sha1 s; s.init(); std::memcpy(gptr(A(c, 0)), &s, sizeof s); RET(c, 0, 1); }
static void XcSHAUpdate(Cpu* c) {
    Sha1 s; std::memcpy(&s, gptr(A(c, 0)), sizeof s);
    s.update((const uint8_t*)gptr(A(c, 1)), A(c, 2));
    std::memcpy(gptr(A(c, 0)), &s, sizeof s);
    if (log_enabled("crypto")) {
        std::string d; char b[3];
        for (uint32_t i = 0; i < std::min(A(c, 2), 96u); i++) { std::snprintf(b, sizeof b, "%02x", R8(A(c, 1) + i)); d += b; }
        hle_log("[crypto] SHA ctx %08x update %u bytes: %s", A(c, 0), A(c, 2), d.c_str());
    }
    RET(c, 0, 3);
}
static void XcSHAFinal(Cpu* c) {
    Sha1 s; std::memcpy(&s, gptr(A(c, 0)), sizeof s);
    s.final((uint8_t*)gptr(A(c, 1)));
    if (log_enabled("crypto")) hle_log("[crypto] SHA ctx %08x final %s", A(c, 0), hex20(A(c, 1)).c_str());
    RET(c, 0, 2);
}
static void hmac_sha1(const void* k, uint32_t kl, const void* d1, uint32_t l1, const void* d2, uint32_t l2, uint8_t* out) {
    uint8_t key[64] = {0};
    if (kl > 64) { Sha1 s; s.init(); s.update((const uint8_t*)k, kl); s.final(key); }
    else std::memcpy(key, k, kl);
    uint8_t ipad[64], opad[64], inner[20];
    for (int i = 0; i < 64; i++) { ipad[i] = key[i] ^ 0x36; opad[i] = key[i] ^ 0x5c; }
    Sha1 s; s.init(); s.update(ipad, 64);
    if (d1) s.update((const uint8_t*)d1, l1);
    if (d2) s.update((const uint8_t*)d2, l2);
    s.final(inner);
    Sha1 o; o.init(); o.update(opad, 64); o.update(inner, 20);
    o.final(out);
}

// Installing a pack stamped its header with flag 1 and a signature keyed to the
// console and title. Packs as served by Xbox Live have neither. Done on the bytes
// the game reads, not on the file; its hash checks of the pack still apply.
static void stamp_content_header(uint8_t* header) {
    header[0x20] |= 1;
    uint8_t key[20];
    hmac_sha1(kHDKey, 16, header + 0x24, 4, nullptr, 0, key);
    hmac_sha1(key, 20, header + 0x14, 0x6c - 0x14, nullptr, 0, header);
}

static void XcHMAC(Cpu* c) {      // (key, keylen, data1, len1, data2, len2, digest)
    uint32_t kl = A(c, 1);
    hmac_sha1(gptr(A(c, 0)), kl, A(c, 2) ? gptr(A(c, 2)) : nullptr, A(c, 3), A(c, 4) ? gptr(A(c, 4)) : nullptr, A(c, 5),
              (uint8_t*)gptr(A(c, 6)));
    if (log_enabled("crypto"))
        hle_log("[crypto] HMAC key %u bytes %s.. data %u+%u -> %s", kl, hex20(A(c, 0)).substr(0, 2 * std::min(kl, 20u)).c_str(),
                A(c, 3), A(c, 5), hex20(A(c, 6)).c_str());
    RET(c, 0, 7);
}

// --- tables ------------------------------------------------------------------------------------------------
struct KFn { const char* name; GuestFn fn; };
#define F(n) {#n, n}
static const KFn kKernelFns[] = {
    F(NtCreateFile), F(NtOpenFile), F(NtReadFile), F(NtWriteFile), F(NtQueryInformationFile),
    F(NtSetInformationFile), F(NtQueryFullAttributesFile), F(NtQueryDirectoryFile),
    F(NtQueryVolumeInformationFile), F(NtFlushBuffersFile), F(NtFsControlFile), F(NtDeviceIoControlFile),
    F(NtReadFileScatter), F(NtWriteFileGather), F(IoCreateSymbolicLink), F(IoDeleteSymbolicLink),
    F(NtOpenSymbolicLinkObject), F(NtQuerySymbolicLinkObject),
    F(NtCreateEvent), F(NtSetEvent), F(NtClearEvent), F(NtPulseEvent), F(NtCreateSemaphore),
    F(NtReleaseSemaphore), F(NtCreateMutant), F(NtReleaseMutant), F(NtCreateTimer), F(NtSetTimerEx),
    F(NtCancelTimer), F(NtWaitForSingleObject), F(NtWaitForSingleObjectEx), F(NtWaitForMultipleObjectsEx),
    F(NtSignalAndWaitForSingleObjectEx), F(KeDelayExecutionThread), F(NtYieldExecution), F(NtClose),
    F(NtDuplicateObject), F(PsCreateSystemThreadEx), F(PsTerminateSystemThread), F(NtSuspendThread),
    F(NtResumeThread), F(KeSetBasePriorityThread), F(KeQueryBasePriorityThread), F(KeSetDisableBoostThread),
    F(NtQueueApcThread), F(ObReferenceObjectByHandle), F(ObfDereferenceObject), F(ObOpenObjectByName),
    F(RtlInitializeCriticalSection), F(RtlEnterCriticalSection), F(RtlLeaveCriticalSection),
    F(KeSetEvent), F(KeWaitForSingleObject), F(KeInitializeTimerEx), F(KeSetTimer), F(KeCancelTimer),
    F(KeInitializeDpc), F(KeInsertQueueDpc), F(KeRemoveQueueDpc), F(KeInitializeInterrupt),
    F(KeConnectInterrupt), F(KeDisconnectInterrupt), F(KeSynchronizeExecution), F(KeStallExecutionProcessor),
    F(KfRaiseIrql), F(KfLowerIrql), F(KeRaiseIrqlToDpcLevel), F(KeSaveFloatingPointState),
    F(KeRestoreFloatingPointState), F(KeBugCheck), F(IofCompleteRequest),
    F(NtAllocateVirtualMemory), F(NtFreeVirtualMemory), F(NtQueryVirtualMemory), F(MmAllocateContiguousMemory),
    F(MmAllocateContiguousMemoryEx), F(MmFreeContiguousMemory), F(MmAllocateSystemMemory),
    F(MmFreeSystemMemory), F(MmQueryAllocationSize), F(MmSetAddressProtect), F(MmQueryAddressProtect),
    F(MmPersistContiguousMemory), F(MmGetPhysicalAddress), F(MmClaimGpuInstanceMemory),
    F(MmLockUnlockBufferPages), F(MmLockUnlockPhysicalPage), F(ExAllocatePool), F(ExAllocatePoolWithTag),
    F(ExFreePool), F(ExQueryPoolBlockSize),
    F(KeQuerySystemTime), F(KeQueryInterruptTime), F(RtlTimeToTimeFields), F(RtlTimeFieldsToTime),
    F(RtlInitAnsiString), F(RtlInitUnicodeString), F(RtlCompareString), F(RtlEqualString), F(RtlFreeAnsiString),
    F(RtlUnicodeStringToAnsiString), F(RtlNtStatusToDosError), F(RtlFillMemoryUlong), F(RtlCompareMemory),
    F(RtlCompareMemoryUlong), F(RtlRaiseException), F(RtlUnwind), F(DbgPrint), F(DbgBreakPoint),
    F(ExQueryNonVolatileSetting), F(HalReturnToFirmware), F(HalInitiateShutdown), F(HalReadWritePCISpace),
    F(HalGetInterruptVector), F(HalRegisterShutdownNotification), F(AvGetSavedDataAddress),
    F(AvSetSavedDataAddress), F(AvSendTVEncoderOption), F(AvSetDisplayMode), F(XeLoadSection),
    F(XeUnloadSection), F(FscSetCacheSize), F(FscGetCacheSize), F(NtCreateIoCompletion), F(NtSetIoCompletion),
    F(NtRemoveIoCompletion), F(NtUserIoApcDispatcher), F(XcSHAInit), F(XcSHAUpdate), F(XcSHAFinal), F(XcHMAC),
};
#undef F

// Data exports: name -> size of the guest object to allocate (initialised below).
static uint32_t g_kdata_next = kKernelData;
static uint32_t kdata(uint32_t size) { uint32_t a = g_kdata_next; g_kdata_next += (size + 15) & ~15u; return a; }

static uint32_t data_export(const std::string& n) {
    if (n == "KeTickCount") { g_tick_count_addr = kdata(4); return g_tick_count_addr; }
    if (n == "KeTimeIncrement") { uint32_t a = kdata(4); W32(a, 10000); return a; }
    if (n == "XboxHardwareInfo") { uint32_t a = kdata(8); W32(a, 0x20); W8(a + 4, 0xa2); W8(a + 5, 0xd3); return a; }
    if (n == "XboxKrnlVersion") { uint32_t a = kdata(8); W16(a, 1); W16(a + 2, 0); W16(a + 4, 5838); W16(a + 6, 1); return a; }
    if (n == "LaunchDataPage") return kdata(4);                         // pointer variable, NULL
    // A real console's HD key is never zero; with a zero key XAPI's save-signing
    // code reads the EEPROM instead. Any fixed value keeps signatures stable.
    if (n == "XboxHDKey") {
        uint32_t a = kdata(16);
        std::memcpy(gptr(a), kHDKey, 16);
        return a;
    }
    if (n == "XboxSignatureKey") return kdata(16);
    if (n == "XboxAlternateSignatureKeys") return kdata(256);
    if (n == "HalBootSMCVideoMode") return kdata(4);
    if (n == "HalDiskCachePartitionCount") { uint32_t a = kdata(4); W32(a, 3); return a; }
    if (n == "IdexChannelObject") return kdata(0x100);
    static const char* types[] = {"ExEventObjectType", "ExSemaphoreObjectType", "ExMutantObjectType",
                                  "ExTimerObjectType", "PsThreadObjectType", "IoFileObjectType"};
    for (int i = 0; i < 6; i++)
        if (n == types[i]) { g_object_types[i] = kdata(0x40); return g_object_types[i]; }
    return 0;
}

#include "kernel_imports.inc"     // { slot address, name } for every import of default.xbe

void kernel_tick() {
    if (g_tick_count_addr) W32(g_tick_count_addr, (uint32_t)((now_100ns() - g_boot_100ns) / 10000));
}

// Patches the kernel thunk table; returns the number of imports without an HLE.
int kernel_install(uint32_t thunk_table) {
    g_boot_100ns = now_100ns();
    int missing = 0;
    for (uint32_t slot = thunk_table; R32(slot); slot += 4) {
        uint32_t ordinal = R32(slot) & 0x7fffffff;
        const char* name = nullptr;
        for (auto& imp : kKernelImports) if (imp.slot == slot) name = imp.name;
        if (!name) { hle_log("[kernel] slot %08x (ordinal %u) has no name", slot, ordinal); missing++; continue; }
        uint32_t d = data_export(name);
        if (d) { W32(slot, d); continue; }
        uint32_t addr = kKernelThunks + ordinal * 16;
        GuestFn fn = nullptr;
        for (auto& k : kKernelFns) if (std::strcmp(k.name, name) == 0) fn = k.fn;
        if (!fn) { hle_log("[kernel] no HLE for %s (ordinal %u)", name, ordinal); missing++; }
        else register_fn(addr, fn);
        W32(slot, addr);
    }
    return missing;
}

}  // namespace tje

namespace tje {
// For HLE libraries: opens a guest path ("d:\...") read-only; returns a handle or 0.
uint32_t open_host_file_for_guest(const std::string& guest_path, uint64_t* size) {
    std::string path = host_path(guest_path);
    if (path.empty()) return 0;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    auto* o = new KObject();
    o->type = ObjType::File;
    o->fp = f;
    o->host_path = path;
    o->guest_path = guest_path;
    std::error_code ec;
    if (size) *size = fs::file_size(path, ec);
    return handle_new(o);
}
}  // namespace tje
