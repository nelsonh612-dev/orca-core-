// Orca Core — the Wii NAND file system as IOS's FS module presents it, stored
// in a host directory.
//
// Rules (public Wii documentation, e.g. WiiBrew "/dev/fs", "Filesystem"):
//   * paths are absolute, at most 63 characters, printable, no trailing '/';
//     names at most 12 characters; directories at most 8 levels deep;
//   * every node has an owner (uid), a group (gid), an attribute byte and
//     read/write permissions for owner, group and others; uid 0 (the
//     kernel) may do anything; otherwise the owner's, else (same gid) the
//     group's, else the others' permission applies;
//   * creating, deleting or renaming needs write permission on the parent
//     directory(ies); listing a directory needs read permission on it;
//     opening a file needs the requested permission on the file;
//   * deleting a directory removes its contents; deleting or renaming an
//     open file (or a directory holding one) fails with "in use";
//   * a file keeps its name when renamed (only its directory may change);
//     an existing node of the same kind at the target is replaced;
//   * a directory lists its newest entries first (the NAND keeps each
//     directory's children in a list with new entries at the front).
// Result codes: -101 invalid, -102 access denied, -105 exists, -106 not
// found, -109 no free handle, -110 too many path components, -111 in use,
// -115 file not empty (owner change of a non-empty file).
//
// Host layout: each NAND node is a host file or directory under the root;
// name characters outside [A-Za-z0-9._-] are stored as %XX. Owners,
// groups, attributes, permissions and creation order are kept in
// "<root>/.orca_nand_meta". Nodes found on the host without an entry there
// (e.g. copied in by the user) get uid 0, gid 0, attribute 0 and read/write
// for everyone.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <map>
#include <span>
#include <optional>
#include <string>
#include <vector>

namespace orca::ios {

namespace fs_err {
inline constexpr s32 kInvalid = -101;
inline constexpr s32 kAccess = -102;
inline constexpr s32 kExists = -105;
inline constexpr s32 kNotFound = -106;
inline constexpr s32 kNoFreeHandle = -109;
inline constexpr s32 kTooDeep = -110;
inline constexpr s32 kInUse = -111;
inline constexpr s32 kNotEmpty = -115;
inline constexpr s32 kUnknown = -117; // host I/O failure
} // namespace fs_err

inline constexpr u8 kFsRead = 1, kFsWrite = 2;

struct FsModes {
    u8 owner{3}, group{3}, other{3};
};

struct FsMeta {
    u32 uid{0};
    u16 gid{0};
    u8 attribute{0};
    FsModes modes{};
    bool is_file{false};
    u32 size{0};
    u64 order{0}; // creation order (higher = newer)
};

class NandFs {
public:
    static constexpr u32 kMaxHandles = 16;
    static constexpr std::size_t kMaxPath = 64, kMaxName = 12, kMaxDepth = 8;
    static constexpr u32 kClusterSize = 0x4000;

    explicit NandFs(std::filesystem::path root);
    ~NandFs();
    NandFs(const NandFs&) = delete;
    NandFs& operator=(const NandFs&) = delete;
    // Creates the root if needed and reads the metadata file.
    bool load(std::string& error);
    const std::filesystem::path& root() const noexcept { return root_; }

    s32 create(u32 uid, u16 gid, const std::string& path, u8 attribute, FsModes modes, bool file);
    s32 remove(u32 uid, u16 gid, const std::string& path);
    s32 rename(u32 uid, u16 gid, const std::string& from, const std::string& to);
    s32 list(u32 uid, u16 gid, const std::string& path, std::vector<std::string>& names);
    s32 metadata(u32 uid, u16 gid, const std::string& path, FsMeta& out);
    s32 set_metadata(u32 caller_uid, const std::string& path, u32 uid, u16 gid, u8 attribute, FsModes modes);
    // Clusters (0x4000 bytes, rounded up per file) and nodes under a directory.
    s32 usage(const std::string& path, u32& clusters, u32& inodes);

    // Files. A handle is >= 0; errors are negative.
    s32 open(u32 uid, u16 gid, const std::string& path, u8 mode);
    s32 close(s32 handle);
    s32 read(s32 handle, std::span<u8> out);       // bytes read
    s32 write(s32 handle, std::span<const u8> in); // bytes written
    s32 seek(s32 handle, s32 offset, u32 whence);  // new position
    s32 status(s32 handle, u32& size, u32& position);
    u32 open_handles() const noexcept;

    static bool valid_path(const std::string& p);           // "/" or a valid non-root path
    static bool valid_non_root(const std::string& p);
    std::filesystem::path host_path(const std::string& nand_path) const;

private:
    struct Handle {
        bool used{false};
        std::string path;
        u8 mode{0};
        u32 position{0};
        std::FILE* file{nullptr};
    };
    FsMeta node(const std::string& path) const; // metadata (defaults if not recorded); host must exist
    bool exists(const std::string& path, bool* is_file = nullptr) const;
    static bool allowed(const FsMeta& m, u32 uid, u16 gid, u8 mode);
    bool path_in_use(const std::string& path, bool prefix) const;
    bool save_meta();
    void drop_meta(const std::string& path); // the node and everything under it

    std::filesystem::path root_;
    std::map<std::string, FsMeta> meta_; // NAND path -> metadata
    u64 next_order_{1};
    std::array<Handle, kMaxHandles> handles_{};
};

} // namespace orca::ios
