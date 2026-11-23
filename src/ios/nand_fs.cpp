#include "orca/ios/nand_fs.hpp"

#include <algorithm>
#include <cinttypes>
#include <fstream>
#include <sstream>
#include <system_error>

namespace orca::ios {

namespace fsys = std::filesystem;

namespace {
constexpr const char* kMetaFile = ".orca_nand_meta";

// Orca's own files in the root; their names are longer than any NAND name.
bool is_internal(const std::string& host_name) {
    return host_name == kMetaFile || host_name == std::string(kMetaFile) + ".new";
}

bool printable(const std::string& s) {
    return std::all_of(s.begin(), s.end(), [](char c) { return c >= 0x20 && c <= 0x7E; });
}

std::string parent_of(const std::string& p) {
    const auto k = p.rfind('/');
    return k == 0 ? std::string("/") : p.substr(0, k);
}
std::string name_of(const std::string& p) { return p.substr(p.rfind('/') + 1); }

std::string escape(const std::string& name) {
    std::string out;
    const bool dots = name == "." || name == "..";
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (!dots && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            out.push_back(c);
        } else {
            char b[4];
            std::snprintf(b, sizeof b, "%%%02X", u);
            out += b;
        }
    }
    return out;
}

std::optional<std::string> unescape(const std::string& host) {
    std::string out;
    for (std::size_t i = 0; i < host.size(); ++i) {
        if (host[i] != '%') { out.push_back(host[i]); continue; }
        if (i + 2 >= host.size()) return std::nullopt;
        unsigned v = 0;
        if (std::sscanf(host.substr(i + 1, 2).c_str(), "%2x", &v) != 1) return std::nullopt;
        out.push_back(static_cast<char>(v));
        i += 2;
    }
    return out;
}
} // namespace

NandFs::NandFs(fsys::path root) : root_(std::move(root)) {}

NandFs::~NandFs() {
    for (auto& h : handles_)
        if (h.file != nullptr) std::fclose(h.file);
}

bool NandFs::valid_non_root(const std::string& p) {
    if (p.size() < 2 || p.size() >= kMaxPath || p.front() != '/' || p.back() == '/' || !printable(p)) return false;
    std::size_t start = 1;
    while (start <= p.size()) {
        const auto end = p.find('/', start);
        const std::size_t len = (end == std::string::npos ? p.size() : end) - start;
        if (len == 0 || len > kMaxName) return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}
bool NandFs::valid_path(const std::string& p) { return p == "/" || valid_non_root(p); }

fsys::path NandFs::host_path(const std::string& p) const {
    fsys::path h = root_;
    std::size_t start = 1;
    while (start < p.size()) {
        auto end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        h /= escape(p.substr(start, end - start));
        start = end + 1;
    }
    return h;
}

bool NandFs::load(std::string& error) {
    std::error_code ec;
    fsys::create_directories(root_, ec);
    if (!fsys::is_directory(root_, ec)) { error = "NAND root " + root_.string() + " is not a directory"; return false; }
    meta_.clear();
    std::ifstream in(root_ / kMetaFile);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        unsigned long long order = 0;
        unsigned uid = 0, gid = 0, attr = 0, mo = 0, mg = 0, mt = 0;
        if (!(ls >> order >> std::hex >> uid >> gid >> attr >> mo >> mg >> mt)) { error = "NAND metadata: bad line: " + line; return false; }
        std::string path;
        ls.get();
        std::getline(ls, path);
        if (!valid_path(path) || gid > 0xFFFFU || attr > 0xFFU || mo > 3U || mg > 3U || mt > 3U) {
            error = "NAND metadata: bad line: " + line;
            return false;
        }
        FsMeta m;
        m.uid = uid;
        m.gid = static_cast<u16>(gid);
        m.attribute = static_cast<u8>(attr);
        m.modes = {static_cast<u8>(mo), static_cast<u8>(mg), static_cast<u8>(mt)};
        m.order = order;
        meta_[path] = m;
        next_order_ = std::max<u64>(next_order_, order + 1U);
    }
    return true;
}

bool NandFs::save_meta() {
    const fsys::path tmp = root_ / (std::string(kMetaFile) + ".new");
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (const auto& [path, m] : meta_) {
            char b[96];
            std::snprintf(b, sizeof b, "%" PRIu64 " %x %x %x %x %x %x ", m.order, m.uid, m.gid, m.attribute, m.modes.owner,
                          m.modes.group, m.modes.other);
            out << b << path << '\n';
        }
        if (!out) return false;
    }
    std::error_code ec;
    fsys::rename(tmp, root_ / kMetaFile, ec);
    return !ec;
}

bool NandFs::exists(const std::string& path, bool* is_file) const {
    std::error_code ec;
    const auto st = fsys::status(host_path(path), ec);
    if (ec || !fsys::exists(st)) return false;
    if (is_file != nullptr) *is_file = fsys::is_regular_file(st);
    return fsys::is_regular_file(st) || fsys::is_directory(st);
}

FsMeta NandFs::node(const std::string& path) const {
    FsMeta m;
    const auto it = meta_.find(path);
    if (it != meta_.end()) m = it->second;
    else if (path == "/") m.modes = {0, kFsRead, kFsRead};
    std::error_code ec;
    const fsys::path h = host_path(path);
    m.is_file = fsys::is_regular_file(h, ec);
    m.size = m.is_file ? static_cast<u32>(std::min<std::uintmax_t>(fsys::file_size(h, ec), 0xFFFFFFFFU)) : 0U;
    return m;
}

bool NandFs::allowed(const FsMeta& m, u32 uid, u16 gid, u8 mode) {
    if (uid == 0U) return true;
    const u8 granted = m.uid == uid ? m.modes.owner : (m.gid == gid ? m.modes.group : m.modes.other);
    return (granted & mode) == mode;
}

bool NandFs::path_in_use(const std::string& path, bool prefix) const {
    for (const auto& h : handles_) {
        if (!h.used) continue;
        if (h.path == path) return true;
        if (prefix && h.path.size() > path.size() && h.path.compare(0, path.size(), path) == 0 && h.path[path.size()] == '/')
            return true;
    }
    return false;
}

void NandFs::drop_meta(const std::string& path) {
    for (auto it = meta_.begin(); it != meta_.end();) {
        const std::string& p = it->first;
        if (p == path || (p.size() > path.size() && p.compare(0, path.size(), path) == 0 && p[path.size()] == '/'))
            it = meta_.erase(it);
        else
            ++it;
    }
}

s32 NandFs::create(u32 uid, u16 gid, const std::string& path, u8 attribute, FsModes modes, bool file) {
    if (!valid_non_root(path) || modes.owner > 3U || modes.group > 3U || modes.other > 3U) return fs_err::kInvalid;
    if (!file && static_cast<std::size_t>(std::count(path.begin(), path.end(), '/')) > kMaxDepth) return fs_err::kTooDeep;
    const std::string parent = parent_of(path);
    bool parent_file = false;
    if (!exists(parent, &parent_file) || parent_file) return fs_err::kNotFound;
    if (!allowed(node(parent), uid, gid, kFsWrite)) return fs_err::kAccess;
    std::error_code ec;
    if (fsys::exists(host_path(path), ec)) return fs_err::kExists;
    if (file) {
        std::FILE* f = std::fopen(host_path(path).string().c_str(), "wb");
        if (f == nullptr) return fs_err::kUnknown;
        std::fclose(f);
    } else if (!fsys::create_directory(host_path(path), ec)) {
        return fs_err::kUnknown;
    }
    FsMeta m;
    m.uid = uid;
    m.gid = gid;
    m.attribute = attribute;
    m.modes = modes;
    m.order = next_order_++;
    meta_[path] = m;
    return save_meta() ? 0 : fs_err::kUnknown;
}

s32 NandFs::remove(u32 uid, u16 gid, const std::string& path) {
    if (!valid_non_root(path)) return fs_err::kInvalid;
    const std::string parent = parent_of(path);
    if (!exists(parent)) return fs_err::kNotFound;
    if (!allowed(node(parent), uid, gid, kFsWrite)) return fs_err::kAccess;
    bool is_file = false;
    if (!exists(path, &is_file)) return fs_err::kNotFound;
    if (path_in_use(path, !is_file)) return fs_err::kInUse;
    std::error_code ec;
    fsys::remove_all(host_path(path), ec);
    if (ec) return fs_err::kUnknown;
    drop_meta(path);
    return save_meta() ? 0 : fs_err::kUnknown;
}

s32 NandFs::rename(u32 uid, u16 gid, const std::string& from, const std::string& to) {
    if (!valid_non_root(from) || !valid_non_root(to)) return fs_err::kInvalid;
    const std::string pf = parent_of(from), pt = parent_of(to);
    bool pt_file = false;
    if (!exists(pf) || !exists(pt, &pt_file) || pt_file) return fs_err::kNotFound;
    if (!allowed(node(pf), uid, gid, kFsWrite) || !allowed(node(pt), uid, gid, kFsWrite)) return fs_err::kAccess;
    bool is_file = false;
    if (!exists(from, &is_file)) return fs_err::kNotFound;
    if (is_file && name_of(from) != name_of(to)) return fs_err::kInvalid;
    if (path_in_use(from, !is_file)) return fs_err::kInUse;
    if (from == to) return 0;
    if (to.size() > from.size() && to.compare(0, from.size(), from) == 0 && to[from.size()] == '/') return fs_err::kInvalid;
    std::error_code ec;
    bool target_file = false;
    if (exists(to, &target_file)) {
        if (target_file != is_file) return fs_err::kInvalid;
        if (path_in_use(to, !target_file)) return fs_err::kInUse;
        fsys::remove_all(host_path(to), ec);
        if (ec) return fs_err::kUnknown;
        drop_meta(to);
    }
    fsys::rename(host_path(from), host_path(to), ec);
    if (ec) return fs_err::kUnknown;
    // move the metadata of the node and its children
    std::map<std::string, FsMeta> moved;
    for (auto it = meta_.begin(); it != meta_.end();) {
        const std::string& p = it->first;
        if (p == from || (p.size() > from.size() && p.compare(0, from.size(), from) == 0 && p[from.size()] == '/')) {
            moved[to + p.substr(from.size())] = it->second;
            it = meta_.erase(it);
        } else {
            ++it;
        }
    }
    meta_.insert(moved.begin(), moved.end());
    return save_meta() ? 0 : fs_err::kUnknown;
}

s32 NandFs::list(u32 uid, u16 gid, const std::string& path, std::vector<std::string>& names) {
    names.clear();
    if (!valid_path(path)) return fs_err::kInvalid;
    bool is_file = false;
    if (!exists(path, &is_file)) return fs_err::kNotFound;
    if (!allowed(node(path), uid, gid, kFsRead)) return fs_err::kAccess;
    if (is_file) return fs_err::kInvalid;
    struct Entry { std::string name; u64 order; bool known; };
    std::vector<Entry> entries;
    std::error_code ec;
    for (const auto& de : fsys::directory_iterator(host_path(path), ec)) {
        const auto n = unescape(de.path().filename().string());
        if (!n || n->empty() || n->size() > kMaxName) continue;
        if (path == "/" && is_internal(de.path().filename().string())) continue;
        const std::string child = (path == "/" ? "/" : path + "/") + *n;
        const auto it = meta_.find(child);
        entries.push_back({*n, it == meta_.end() ? 0U : it->second.order, it != meta_.end()});
    }
    if (ec) return fs_err::kUnknown;
    // newest first; entries without a recorded order first, by name (descending), for a stable result
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.known != b.known) return !a.known;
        if (a.order != b.order) return a.order > b.order;
        return a.name > b.name;
    });
    for (auto& e : entries) names.push_back(std::move(e.name));
    return 0;
}

s32 NandFs::metadata(u32 uid, u16 gid, const std::string& path, FsMeta& out) {
    if (!valid_path(path)) return fs_err::kInvalid;
    if (path != "/") {
        const std::string parent = parent_of(path);
        if (!exists(parent)) return fs_err::kNotFound;
        if (!allowed(node(parent), uid, gid, kFsRead)) return fs_err::kAccess;
    }
    if (!exists(path)) return fs_err::kNotFound;
    out = node(path);
    return 0;
}

s32 NandFs::set_metadata(u32 caller_uid, const std::string& path, u32 uid, u16 gid, u8 attribute, FsModes modes) {
    if (!valid_path(path) || modes.owner > 3U || modes.group > 3U || modes.other > 3U) return fs_err::kInvalid;
    if (!exists(path)) return fs_err::kNotFound;
    FsMeta m = node(path);
    if (caller_uid != 0U && caller_uid != m.uid) return fs_err::kAccess;
    if (caller_uid != 0U && uid != m.uid) return fs_err::kAccess;
    if (m.uid != uid && m.is_file && m.size != 0U) return fs_err::kNotEmpty;
    m.uid = uid;
    m.gid = gid;
    m.attribute = attribute;
    m.modes = modes;
    if (m.order == 0U) m.order = next_order_++;
    meta_[path] = m;
    return save_meta() ? 0 : fs_err::kUnknown;
}

s32 NandFs::usage(const std::string& path, u32& clusters, u32& inodes) {
    if (!valid_path(path)) return fs_err::kInvalid;
    bool is_file = false;
    if (!exists(path, &is_file)) return fs_err::kNotFound;
    if (is_file) return fs_err::kInvalid;
    clusters = 0;
    inodes = 1; // the directory itself
    std::error_code ec;
    for (auto it = fsys::recursive_directory_iterator(host_path(path), ec); !ec && it != fsys::recursive_directory_iterator();
         it.increment(ec)) {
        if (path == "/" && it.depth() == 0 && is_internal(it->path().filename().string())) continue;
        ++inodes;
        if (it->is_regular_file()) {
            const auto size = it->file_size(ec);
            clusters += static_cast<u32>((size + kClusterSize - 1U) / kClusterSize);
        }
    }
    return ec ? fs_err::kUnknown : 0;
}

s32 NandFs::open(u32 uid, u16 gid, const std::string& path, u8 mode) {
    if (!valid_non_root(path) || mode > 3U) return fs_err::kInvalid;
    bool is_file = false;
    if (!exists(path, &is_file)) return fs_err::kNotFound;
    if (!is_file) return fs_err::kInvalid;
    if (!allowed(node(path), uid, gid, mode)) return fs_err::kAccess;
    for (s32 i = 0; i < static_cast<s32>(kMaxHandles); ++i) {
        Handle& h = handles_[static_cast<std::size_t>(i)];
        if (h.used) continue;
        h.file = std::fopen(host_path(path).string().c_str(), "r+b");
        if (h.file == nullptr) return fs_err::kUnknown;
        h.used = true;
        h.path = path;
        h.mode = mode;
        h.position = 0;
        return i;
    }
    return fs_err::kNoFreeHandle;
}

s32 NandFs::close(s32 handle) {
    if (handle < 0 || handle >= static_cast<s32>(kMaxHandles) || !handles_[static_cast<std::size_t>(handle)].used)
        return fs_err::kInvalid;
    Handle& h = handles_[static_cast<std::size_t>(handle)];
    const bool ok = std::fclose(h.file) == 0;
    h = Handle{};
    return ok ? 0 : fs_err::kUnknown;
}

s32 NandFs::read(s32 handle, std::span<u8> out) {
    if (handle < 0 || handle >= static_cast<s32>(kMaxHandles) || !handles_[static_cast<std::size_t>(handle)].used)
        return fs_err::kInvalid;
    Handle& h = handles_[static_cast<std::size_t>(handle)];
    if ((h.mode & kFsRead) == 0U) return fs_err::kAccess;
    u32 size = 0, pos = 0;
    status(handle, size, pos);
    const u32 n = pos >= size ? 0U : static_cast<u32>(std::min<std::size_t>(out.size(), size - pos));
    if (n != 0U) {
        if (std::fseek(h.file, static_cast<long>(pos), SEEK_SET) != 0 || std::fread(out.data(), 1, n, h.file) != n)
            return fs_err::kUnknown;
    }
    h.position = pos + n;
    return static_cast<s32>(n);
}

s32 NandFs::write(s32 handle, std::span<const u8> in) {
    if (handle < 0 || handle >= static_cast<s32>(kMaxHandles) || !handles_[static_cast<std::size_t>(handle)].used)
        return fs_err::kInvalid;
    Handle& h = handles_[static_cast<std::size_t>(handle)];
    if ((h.mode & kFsWrite) == 0U) return fs_err::kAccess;
    if (in.size() > 0x7FFFFFFFU - h.position) return fs_err::kInvalid;
    if (!in.empty()) {
        if (std::fseek(h.file, static_cast<long>(h.position), SEEK_SET) != 0 ||
            std::fwrite(in.data(), 1, in.size(), h.file) != in.size() || std::fflush(h.file) != 0)
            return fs_err::kUnknown;
    }
    h.position += static_cast<u32>(in.size());
    return static_cast<s32>(in.size());
}

s32 NandFs::seek(s32 handle, s32 offset, u32 whence) {
    u32 size = 0, pos = 0;
    const s32 st = status(handle, size, pos);
    if (st < 0) return st;
    s64 base = 0;
    switch (whence) {
    case 0: base = 0; break;
    case 1: base = pos; break;
    case 2: base = size; break;
    default: return fs_err::kInvalid;
    }
    const s64 target = base + offset;
    if (target < 0 || target > static_cast<s64>(size)) return fs_err::kInvalid;
    handles_[static_cast<std::size_t>(handle)].position = static_cast<u32>(target);
    return static_cast<s32>(target);
}

s32 NandFs::status(s32 handle, u32& size, u32& position) {
    if (handle < 0 || handle >= static_cast<s32>(kMaxHandles) || !handles_[static_cast<std::size_t>(handle)].used)
        return fs_err::kInvalid;
    Handle& h = handles_[static_cast<std::size_t>(handle)];
    if (std::fseek(h.file, 0, SEEK_END) != 0) return fs_err::kUnknown;
    const long end = std::ftell(h.file);
    if (end < 0) return fs_err::kUnknown;
    size = static_cast<u32>(end);
    position = h.position;
    return 0;
}

u32 NandFs::open_handles() const noexcept {
    return static_cast<u32>(std::count_if(handles_.begin(), handles_.end(), [](const Handle& h) { return h.used; }));
}

} // namespace orca::ios
