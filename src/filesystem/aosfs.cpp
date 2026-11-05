/* SPDX-License-Identifier: MIT */
// aosfs.cpp - Real persistent filesystem on ATA (cluster chain)
//
// Layout (all sizes in 512-byte sectors / clusters):
//   0          Superblock
//   1..fat_sz  FAT (uint32_t next-cluster; 0=free, 0xFFFFFFFF=EOF)
//   fat_sz+1   Root directory cluster (inode 0 is virtual root)
//   ...        Data clusters
//
// Directory entry (32 bytes, packed):
//   char     name[22];   // NUL-terminated, max 21 chars (MAX_NAME)
//   uint8_t  type;       // 0=empty, 1=file, 2=dir
//   uint8_t  reserved;
//   uint32_t size;       // bytes
//   uint32_t first;      // first cluster (0 = empty file)
//   // 22+1+1+4+4 = 32 — no padding
//
// Root is a fixed directory stored starting at cluster `root_cluster`.
// Subdirectories are files whose content is an array of DirEnt.
#include "aosfs.hpp"
#include "page_cache.hpp"
#include "../drivers/storage-drivers/ata_pio.hpp"
#include "../arch/x86_64/interrupts.hpp"
#include "../memory/physical/pmm.h"

namespace aos::aosfs {
namespace {

using aos::fs::Result;
using aos::fs::Node;
using aos::fs::NAME_MAX;
using aos::fs::CONTENT_MAX;

constexpr uint32_t MAGIC     = 0x53464F41u; // 'AOFS'
constexpr uint32_t VERSION   = 1;
constexpr uint32_t FAT_FREE  = 0;
constexpr uint32_t FAT_EOF   = 0xFFFFFFFFu;
constexpr uint32_t FAT_BAD   = 0xFFFFFFFEu;
constexpr size_t   DIRENT_SZ = 32;
constexpr size_t   DIR_ENTS_PER_CLUSTER = 512 / DIRENT_SZ; // 16
constexpr uint32_t MAX_NAME  = 21;
constexpr uint32_t MAX_CLUSTERS = 4u * 1024 * 1024;   // 2 GiB of 512-byte clusters (FAT = 16 MiB in RAM)

#pragma pack(push, 1)
struct Superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t total_clusters;   // whole disk in 512B units
    uint32_t fat_start;        // usually 1
    uint32_t fat_clusters;     // number of sectors for FAT
    uint32_t root_cluster;     // first cluster of root directory
    uint32_t data_start;       // first data cluster index (= root_cluster)
    uint32_t free_clusters;
    uint32_t cluster_count;    // data clusters usable (from data_start to end)
    char     label[32];
    uint8_t  pad[512 - 36 - 32];
};
static_assert(sizeof(Superblock) == 512, "superblock must be one sector");

struct DirEnt {
    char     name[22];   // max 21 chars + NUL
    uint8_t  type;       // 0 empty, 1 file, 2 dir
    uint8_t  reserved;
    uint32_t size;
    uint32_t first;
};
static_assert(sizeof(DirEnt) == 32, "dirent must be 32 bytes");
#pragma pack(pop)

const aos::storage::BlockDevice* g_dev = nullptr;
Superblock g_sb{};
uint32_t*  g_fat = nullptr;          // in-memory FAT (allocated once)
uint32_t   g_fat_entries = 0;
uint32_t   g_cwd_cluster = 0;        // directory cluster we are "in"

// In-memory path stack used for fast `cd ..` and to refuse removing a directory
// that is on the current path. New directories are seeded with on-disk "." / ".."
// (see mkdir_one / format); the stack remains the authority for nested navigation
// so that directories created before that fix still work.
constexpr int MAX_PATH_DEPTH = 16;
constexpr int MAX_DIR_ENTS = 128;  // 4 KiB on stack; was 512 (16 KiB)
struct PathComp {
    uint32_t cluster;   // first cluster of this directory
    char     name[22];  // its name relative to the parent
};
PathComp g_path[MAX_PATH_DEPTH]{};
int      g_path_depth = 0;   // 0 == at root

[[maybe_unused]] void cwd_reset_to_root() {
    g_cwd_cluster = g_sb.root_cluster;
    g_path_depth  = 0;
}

bool       g_mounted = false;
bool       g_fat_dirty = false;
uint8_t    g_fat_dirty_bits[MAX_CLUSTERS / 128 / 8 + 8];   // one bit per FAT sector
uint32_t   g_alloc_hint = 0;
uint32_t   g_fat_cap_sectors = 0;
inline void fat_mark(uint32_t c) { const uint32_t sct = c / 128; g_fat_dirty_bits[sct >> 3] |= static_cast<uint8_t>(1u << (sct & 7)); g_fat_dirty = true; }
inline void fat_set(uint32_t c, uint32_t v) { g_fat[c] = v; fat_mark(c); }

// read buffer for fs::read()
Node g_read_node{};

// ---------- block I/O (with page cache) ----------
// Device identity for the cache: non-zero pointer-derived id.
inline uint64_t cache_dev_id() {
    return reinterpret_cast<uint64_t>(g_dev) | 1ULL;
}

bool read_cluster(uint32_t cluster, void* buf) {
    if (!g_dev || !buf) return false;
    if (cluster >= g_sb.total_clusters || cluster >= g_fat_entries) return false;
    const uint64_t id = cache_dev_id();
    if (aos::page_cache::get(id, cluster, buf)) return true;
    if (!aos::storage::read_blocks(g_dev, cluster, 1, buf)) return false;
    aos::page_cache::put(id, cluster, buf);
    return true;
}
bool write_cluster(uint32_t cluster, const void* buf) {
    if (!g_dev || !g_dev->write || !buf) return false;
    if (cluster >= g_sb.total_clusters || cluster >= g_fat_entries) return false;
    if (!aos::storage::write_blocks(g_dev, cluster, 1, buf)) return false;
    // Keep cache coherent: either update or drop the entry.
    const uint64_t id = cache_dev_id();
    aos::page_cache::put(id, cluster, buf);
    return true;
}

bool load_fat() {
    if (!g_fat) return false;
    uint32_t done = 0;
    while (done < g_sb.fat_clusters) {                      // 64 KiB per command
        uint32_t n = g_sb.fat_clusters - done;
        if (n > 128) n = 128;
        if (!aos::storage::read_blocks(g_dev, g_sb.fat_start + done, n, reinterpret_cast<uint8_t*>(g_fat) + static_cast<uint64_t>(done) * 512)) return false;
        done += n;
    }
    for (uint32_t i = 0; i < sizeof g_fat_dirty_bits; ++i) g_fat_dirty_bits[i] = 0;
    g_fat_dirty = false;
    return true;
}

bool flush_fat() {
    if (!g_fat_dirty || !g_fat) return true;
    uint32_t i = 0;
    while (i < g_sb.fat_clusters) {                          // write only the dirty FAT sectors, coalesced into runs
        if (!(g_fat_dirty_bits[i >> 3] & (1u << (i & 7)))) { ++i; continue; }
        uint32_t run = 1;
        while (i + run < g_sb.fat_clusters && run < 128 && (g_fat_dirty_bits[(i + run) >> 3] & (1u << ((i + run) & 7)))) ++run;
        if (!aos::storage::write_blocks(g_dev, g_sb.fat_start + i, run, reinterpret_cast<uint8_t*>(g_fat) + static_cast<uint64_t>(i) * 512)) return false;
        for (uint32_t k = 0; k < run; ++k) g_fat_dirty_bits[(i + k) >> 3] &= static_cast<uint8_t>(~(1u << ((i + k) & 7)));
        i += run;
    }
    g_fat_dirty = false;
    return true;
}

bool flush_sb() {
    return write_cluster(0, &g_sb);
}

// ---------- cluster allocation ----------
uint32_t alloc_cluster() {
    // data clusters are indices from data_start .. total_clusters-1; FAT is indexed by absolute cluster number
    if (g_alloc_hint < g_sb.data_start || g_alloc_hint >= g_sb.total_clusters) g_alloc_hint = g_sb.data_start;
    uint32_t c = g_alloc_hint;
    for (uint32_t n = g_sb.data_start; n < g_sb.total_clusters; ++n) {
        if (c >= g_sb.total_clusters) c = g_sb.data_start;
        if (g_fat[c] == FAT_FREE) {
            fat_set(c, FAT_EOF);
            if (g_sb.free_clusters) --g_sb.free_clusters;
            g_alloc_hint = c + 1;
            return c;
        }
        ++c;
    }
    return 0; // 0 is reserved / failure (cluster 0 is superblock)
}

void free_chain(uint32_t first) {
    // Walk at most total_clusters steps so a corrupt cyclic FAT cannot hang
    // the kernel or inflate free_clusters without bound.
    uint32_t c = first;
    uint32_t steps = 0;
    const uint32_t limit = g_sb.total_clusters ? g_sb.total_clusters : 1;
    while (c != 0 && c != FAT_EOF && c != FAT_BAD && c < g_fat_entries && steps < limit) {
        ++steps;
        const uint32_t next = g_fat[c];
        // Only credit free_clusters when we actually transition a used slot
        // to free — protects against double-free of the same chain.
        if (g_fat[c] != FAT_FREE) {
            fat_set(c, FAT_FREE);
            if (g_sb.free_clusters < g_sb.cluster_count)
                ++g_sb.free_clusters;
        }
        if (next == FAT_EOF || next == FAT_FREE || next == FAT_BAD) break;
        c = next;
    }
}

[[maybe_unused]] uint32_t chain_length(uint32_t first) {
    uint32_t n = 0, c = first;
    while (c && c != FAT_EOF && c < g_fat_entries && n < g_sb.total_clusters) {
        ++n;
        c = g_fat[c];
        if (c == FAT_EOF) break;
    }
    return n;
}

// ---------- name helpers ----------
// Forward decls (path resolution calls these; defined below).
int load_dir(uint32_t dir_cluster, DirEnt* out, int max_ents);
int find_ent(const DirEnt* ents, int n, const char* name);
bool write_ent_back(uint32_t dir_cluster, int index, const DirEnt& ent);
int find_free_slot(uint32_t dir_cluster, DirEnt* ents, int& n, int max_ents);

bool name_ok(const char* name) {
    if (!name || !*name) return false;
    size_t n = 0;
    while (name[n]) {
        if (n >= MAX_NAME) return false;
        char c = name[n];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-';
        if (!ok) return false;
        ++n;
    }
    if (name[0] == '.') return false;
    return true;
}

bool name_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) { ++a; ++b; }
    return *a == *b;
}

void name_copy(char* dst, const char* src, size_t max) {
    size_t i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

// True for the special directory links we plant in every new folder.
bool is_dot_or_dotdot(const char* name) {
    if (!name || !name[0]) return false;
    if (name[0] == '.' && name[1] == 0) return true;
    if (name[0] == '.' && name[1] == '.' && name[2] == 0) return true;
    return false;
}

// ---------- path resolution ----------
// Split and walk absolute or relative paths: "a/b", "/a/../b", ".."
// Parent lookup returns the directory that CONTAINS the final component.

constexpr int MAX_PATH_COMPONENTS = MAX_PATH_DEPTH + 4;

struct Resolved {
    uint32_t dir_cluster;   // parent directory cluster for the final leaf
    char     leaf[22];      // final component (empty if path ends at a directory itself)
    uint32_t target_cluster;// if path names an existing dir, its first cluster; else 0
    uint8_t  target_type;   // 0 none, 1 file, 2 dir
    bool     is_root;       // path resolved to filesystem root
};

bool valid_data_cluster(uint32_t c) {
    return c >= g_sb.data_start && c < g_sb.total_clusters && c < g_fat_entries
        && g_fat[c] != FAT_BAD;
}

// Walk one component from `dir`. On success sets *out_first and *out_type.
// "." and ".." are valid names here (seeded by mkdir/format); other names
// starting with '.' are rejected by name_ok() at the resolve/mkdir layer.
Result lookup_one(uint32_t dir, const char* comp, uint32_t* out_first, uint8_t* out_type) {
    if (!dir || !comp || !comp[0]) return Result::BadName;
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(dir, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int idx = find_ent(ents, n, comp);
    if (idx < 0) return Result::NotFound;
    if (out_first) *out_first = ents[idx].first;
    if (out_type)  *out_type  = ents[idx].type;
    return Result::Ok;
}

// Resolve `path` relative to cwd (or absolute if starts with '/').
// mode_leaf_parent: if true, stop at parent of last component (for create/mkdir/remove).
//                   if false, fully resolve existing nodes (for cd/read of existing).
Result resolve_path(const char* path, bool leaf_parent, Resolved* out) {
    if (!out || !path) return Result::BadName;
    *out = {};
    out->dir_cluster = g_cwd_cluster ? g_cwd_cluster : g_sb.root_cluster;

    // Empty path
    if (!path[0]) return Result::BadName;

    // Copy path to mutable buffer (max ~192)
    char buf[192];
    size_t plen = 0;
    while (path[plen] && plen + 1 < sizeof(buf)) { buf[plen] = path[plen]; ++plen; }
    if (path[plen]) return Result::BadName; // too long
    buf[plen] = 0;

    // Absolute?
    size_t i = 0;
    int depth = g_path_depth;
    uint32_t stack_c[MAX_PATH_DEPTH + 4];
    char     stack_n[MAX_PATH_DEPTH + 4][22];
    if (buf[0] == '/') {
        out->dir_cluster = g_sb.root_cluster;
        depth = 0;
        i = 1;
        if (!buf[1]) {
            out->is_root = true;
            out->target_cluster = g_sb.root_cluster;
            out->target_type = 2;
            out->leaf[0] = 0;
            return Result::Ok;
        }
    } else {
        // relative: seed stack from g_path
        for (int k = 0; k < g_path_depth && k < MAX_PATH_DEPTH; ++k) {
            stack_c[k] = g_path[k].cluster;
            name_copy(stack_n[k], g_path[k].name, sizeof(stack_n[k]));
        }
        depth = g_path_depth;
        out->dir_cluster = depth ? stack_c[depth - 1] : g_sb.root_cluster;
    }

    // Tokenize on '/'
    while (buf[i]) {
        while (buf[i] == '/') ++i;
        if (!buf[i]) break;
        char comp[22];
        size_t cl = 0;
        while (buf[i] && buf[i] != '/') {
            if (cl + 1 >= sizeof(comp)) return Result::BadName;
            comp[cl++] = buf[i++];
        }
        comp[cl] = 0;

        if (comp[0] == '.' && comp[1] == 0)
            continue; // "."

        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (depth > 0) {
                --depth;
                out->dir_cluster = depth ? stack_c[depth - 1] : g_sb.root_cluster;
            } else {
                out->dir_cluster = g_sb.root_cluster;
            }
            out->leaf[0] = 0;
            out->target_cluster = out->dir_cluster;
            out->target_type = 2;
            continue;
        }

        if (!name_ok(comp)) return Result::BadName;

        // Peek if more components follow
        size_t j = i;
        while (buf[j] == '/') ++j;
        const bool more = buf[j] != 0;

        if (leaf_parent && !more) {
            // Final component: parent dir + leaf name. Existence is optional.
            name_copy(out->leaf, comp, sizeof(out->leaf));
            out->target_cluster = 0;
            out->target_type = 0;
            uint32_t first = 0; uint8_t ty = 0;
            Result r = lookup_one(out->dir_cluster, comp, &first, &ty);
            if (r == Result::Ok) {
                out->target_cluster = first;
                out->target_type = ty;
            } else if (r != Result::NotFound) {
                return r;
            }
            return Result::Ok;
        }

        // Must exist and be a directory to descend
        uint32_t first = 0; uint8_t ty = 0;
        Result r = lookup_one(out->dir_cluster, comp, &first, &ty);
        if (r != Result::Ok) return r;
        if (ty != 2 || !first || !valid_data_cluster(first)) {
            if (ty == 1) return Result::NotDir;
            return Result::IoError;
        }
        if (depth >= MAX_PATH_DEPTH) return Result::Full;
        stack_c[depth] = first;
        name_copy(stack_n[depth], comp, sizeof(stack_n[depth]));
        ++depth;
        out->dir_cluster = first;
        name_copy(out->leaf, comp, sizeof(out->leaf));
        out->target_cluster = first;
        out->target_type = 2;
    }

    if (!out->leaf[0] && depth == 0) {
        out->is_root = true;
        out->target_cluster = g_sb.root_cluster;
        out->target_type = 2;
    }
    return Result::Ok;
}

// Apply a successful cd onto the path stack by re-walking absolute path from root.
// Simpler: rebuild stack from Resolved by walking from root when absolute, else mutate.
Result apply_cd_to_stack(const char* path) {
    // Re-resolve and rebuild g_path by walking components from root or relative rules.
    char buf[192];
    size_t plen = 0;
    while (path[plen] && plen + 1 < sizeof(buf)) { buf[plen] = path[plen]; ++plen; }
    buf[plen] = 0;

    int depth = 0;
    uint32_t cur = g_sb.root_cluster;
    PathComp tmp[MAX_PATH_DEPTH]{};

    size_t i = 0;
    if (buf[0] == '/') {
        i = 1;
        depth = 0;
        cur = g_sb.root_cluster;
    } else {
        depth = g_path_depth;
        for (int k = 0; k < depth; ++k) tmp[k] = g_path[k];
        cur = depth ? tmp[depth - 1].cluster : g_sb.root_cluster;
    }

    while (buf[i]) {
        while (buf[i] == '/') ++i;
        if (!buf[i]) break;
        char comp[22];
        size_t cl = 0;
        while (buf[i] && buf[i] != '/') {
            if (cl + 1 >= sizeof(comp)) return Result::BadName;
            comp[cl++] = buf[i++];
        }
        comp[cl] = 0;

        if (comp[0]=='.' && comp[1]==0) continue;
        if (comp[0]=='.' && comp[1]=='.' && comp[2]==0) {
            if (depth > 0) --depth;
            cur = depth ? tmp[depth - 1].cluster : g_sb.root_cluster;
            continue;
        }
        uint32_t first = 0; uint8_t ty = 0;
        Result r = lookup_one(cur, comp, &first, &ty);
        if (r != Result::Ok) return r;
        if (ty != 2 || !first) return Result::NotDir;
        if (depth >= MAX_PATH_DEPTH) return Result::Full;
        tmp[depth].cluster = first;
        name_copy(tmp[depth].name, comp, sizeof(tmp[depth].name));
        ++depth;
        cur = first;
    }

    for (int k = 0; k < depth; ++k) g_path[k] = tmp[k];
    g_path_depth = depth;
    g_cwd_cluster = cur;
    return Result::Ok;
}

// ---------- directory helpers ----------
// Load all dirents from a directory starting at `dir_cluster` into a temp buffer.
// Returns number of entries (including empty slots), or -1 on I/O error.
// max_ents limits how many we read.
int load_dir(uint32_t dir_cluster, DirEnt* out, int max_ents) {
    if (!dir_cluster || max_ents <= 0) return -1;
    int count = 0;
    uint32_t c = dir_cluster;
    uint8_t sector[512];
    while (c && c != FAT_EOF && c < g_fat_entries && count < max_ents) {
        if (!read_cluster(c, sector)) return -1;
        for (size_t i = 0; i < DIR_ENTS_PER_CLUSTER && count < max_ents; ++i) {
            __builtin_memcpy(&out[count], sector + i * DIRENT_SZ, DIRENT_SZ);
            ++count;
        }
        uint32_t next = g_fat[c];
        if (next == FAT_EOF || next == FAT_FREE) break;
        c = next;
    }
    return count;
}

[[maybe_unused]] bool save_dir_cluster(uint32_t cluster, const DirEnt* ents, int n_in_cluster) {
    uint8_t sector[512]{};
    for (int i = 0; i < n_in_cluster && i < (int)DIR_ENTS_PER_CLUSTER; ++i)
        __builtin_memcpy(sector + i * DIRENT_SZ, &ents[i], DIRENT_SZ);
    return write_cluster(cluster, sector);
}

// Find entry by name in directory. Returns index in loaded array, or -1.
int find_ent(const DirEnt* ents, int n, const char* name) {
    for (int i = 0; i < n; ++i)
        if (ents[i].type != 0 && name_eq(ents[i].name, name))
            return i;
    return -1;
}

// Find free slot; if directory full, try to extend chain. Returns index or -1.
int find_free_slot(uint32_t dir_cluster, DirEnt* ents, int& n, int max_ents) {
    for (int i = 0; i < n; ++i)
        if (ents[i].type == 0) return i;
    // need to grow directory
    if (n + (int)DIR_ENTS_PER_CLUSTER > max_ents) return -1;
    // find last cluster of dir
    uint32_t c = dir_cluster;
    while (g_fat[c] != FAT_EOF && g_fat[c] != FAT_FREE && g_fat[c] < g_fat_entries)
        c = g_fat[c];
    uint32_t neu = alloc_cluster();
    if (!neu) return -1;
    fat_set(c, neu);
    fat_set(neu, FAT_EOF);
    // zero new cluster entries in memory
    for (size_t i = 0; i < DIR_ENTS_PER_CLUSTER; ++i)
        ents[n + (int)i] = {};
    // write empty cluster
    uint8_t z[512]{};
    if (!write_cluster(neu, z)) return -1;
    int slot = n;
    n += (int)DIR_ENTS_PER_CLUSTER;
    return slot;
}

bool write_ent_back(uint32_t dir_cluster, int index, const DirEnt& ent) {
    // walk to the cluster containing this index
    int cluster_idx = index / (int)DIR_ENTS_PER_CLUSTER;
    int slot = index % (int)DIR_ENTS_PER_CLUSTER;
    uint32_t c = dir_cluster;
    for (int i = 0; i < cluster_idx; ++i) {
        if (c == 0 || c == FAT_EOF || c >= g_fat_entries) return false;
        c = g_fat[c];
    }
    if (c == 0 || c == FAT_EOF || c >= g_fat_entries) return false;
    uint8_t sector[512];
    if (!read_cluster(c, sector)) return false;
    __builtin_memcpy(sector + slot * DIRENT_SZ, &ent, DIRENT_SZ);
    return write_cluster(c, sector);
}

// Read entire file chain into buf (up to max bytes). Returns bytes read.
size_t read_file_data(uint32_t first, uint32_t size, uint8_t* buf, size_t max) {
    size_t got = 0;
    uint32_t c = first;
    uint8_t sector[512];
    while (c && c != FAT_EOF && c < g_fat_entries && got < size && got < max) {
        if (!read_cluster(c, sector)) break;
        size_t need = size - got;
        if (need > 512) need = 512;
        if (got + need > max) need = max - got;
        for (size_t i = 0; i < need; ++i) buf[got + i] = sector[i];
        got += need;
        if (got >= size || got >= max) break;
        uint32_t next = g_fat[c];
        if (next == FAT_EOF) break;
        c = next;
    }
    return got;
}

// Write data to a new or existing chain. Returns first cluster, 0 on failure.
// If *first_in_out is non-zero, reuses/frees old chain as needed.
// Allocate a brand-new chain and write `len` bytes. Does NOT free any prior chain.
// On success *first_in_out receives the first cluster; on failure returns 0 and frees the partial new chain.
uint32_t write_file_data(uint32_t* first_in_out, const uint8_t* data, size_t len) {
    if (len == 0) {
        if (first_in_out) *first_in_out = 0;
        return 0;
    }
    if (!data) return 0;
    size_t remaining = len;
    const uint8_t* p = data;
    uint32_t first = 0, prev = 0;
    while (remaining > 0) {
        uint32_t c = alloc_cluster();
        if (!c) {
            if (first) free_chain(first);
            return 0;
        }
        uint8_t sector[512]{};
        size_t chunk = remaining > 512 ? 512 : remaining;
        for (size_t i = 0; i < chunk; ++i) sector[i] = p[i];
        if (!write_cluster(c, sector)) {
            free_chain(first ? first : c);
            if (first && c != first) { fat_set(c, FAT_FREE); }
            return 0;
        }
        if (!first) first = c;
        if (prev) { fat_set(prev, c); }
        fat_set(c, FAT_EOF);
        prev = c;
        p += chunk;
        remaining -= chunk;
    }
    if (first_in_out) *first_in_out = first;
    return first;
}

// ---------- path / cwd ----------
 // max entries we load at once (~16KB)

}  // namespace

bool mounted() { return g_mounted; }

void unmount() {
    if (g_mounted) {
        flush_fat();
        flush_sb();
        if (g_dev) aos::page_cache::invalidate_device(cache_dev_id());
    }
    g_mounted = false;
    g_dev = nullptr;
    // leave g_fat allocated for possible remount
}

bool format(const aos::storage::BlockDevice* dev) {
    if (!dev || !aos::storage::valid(dev) || !dev->write) return false;
    if (dev->block_size != 512) return false;
    if (dev->block_count < 64) return false; // too small
    aos::page_cache::invalidate_device(reinterpret_cast<uint64_t>(dev) | 1ULL);

    const uint32_t total = static_cast<uint32_t>(dev->block_count > MAX_CLUSTERS ? MAX_CLUSTERS : dev->block_count);

    // FAT needs 4 bytes per cluster
    uint32_t fat_clusters = (total * 4 + 511) / 512;
    if (fat_clusters < 1) fat_clusters = 1;

    Superblock sb{};
    sb.magic = MAGIC;
    sb.version = VERSION;
    sb.total_clusters = total;
    sb.fat_start = 1;
    sb.fat_clusters = fat_clusters;
    sb.root_cluster = 1 + fat_clusters;
    sb.data_start = sb.root_cluster; // root is first data cluster
    sb.cluster_count = total - sb.data_start;
    sb.free_clusters = sb.cluster_count - 1; // root takes one
    name_copy(sb.label, "AOSFS", 32);

    {   // invalidate any old superblock first: an interrupted format must never look like a valid filesystem
        uint8_t z[512]{};
        if (!aos::storage::write_blocks(dev, 0, 1, z)) return false;
    }

    // FAT: clusters 0..root-1 are reserved (superblock + FAT itself), root = EOF, everything else free.
    // Written in 64 KiB commands. The superblock goes LAST so a half-formatted disk is never mistaken for AOSFS.
    static uint32_t fatbuf[128 * 128];
    uint32_t done = 0;
    while (done < fat_clusters) {
        uint32_t n = fat_clusters - done;
        if (n > 128) n = 128;
        for (uint32_t e = 0; e < n * 128; ++e) {
            const uint32_t idx = done * 128 + e;
            fatbuf[e] = idx < sb.root_cluster ? FAT_BAD : (idx == sb.root_cluster ? FAT_EOF : FAT_FREE);
        }
        if (!aos::storage::write_blocks(dev, sb.fat_start + done, n, fatbuf)) return false;
        done += n;
    }

    // Root directory seeded with "." and ".." (both point at root).
    DirEnt root_ents[DIR_ENTS_PER_CLUSTER]{};
    name_copy(root_ents[0].name, ".", sizeof(root_ents[0].name));
    root_ents[0].type  = 2;
    root_ents[0].first = sb.root_cluster;
    name_copy(root_ents[1].name, "..", sizeof(root_ents[1].name));
    root_ents[1].type  = 2;
    root_ents[1].first = sb.root_cluster;
    if (!aos::storage::write_blocks(dev, sb.root_cluster, 1, root_ents)) return false;

    // Superblock last — a half-formatted disk must never look valid.
    if (!aos::storage::write_blocks(dev, 0, 1, &sb)) return false;
    aos::storage::flush_blocks(dev);
    return true;
}

bool mount(const aos::storage::BlockDevice* dev) {
    if (!dev || !aos::storage::valid(dev) || dev->block_size != 512) return false;
    Superblock sb{};
    if (!aos::storage::read_blocks(dev, 0, 1, &sb)) return false;
    if (sb.magic != MAGIC || sb.version != VERSION) return false;
    if (sb.total_clusters < 64 || sb.fat_start == 0 || sb.fat_clusters == 0) return false;
    if (sb.root_cluster < sb.fat_start + sb.fat_clusters) return false;
    if (sb.total_clusters > dev->block_count) return false;

    if (sb.total_clusters > MAX_CLUSTERS) return false;
    // FAT lives in RAM (physically contiguous pages, allocated once and reused on remount)
    const uint32_t need_sectors = sb.fat_clusters;
    if (need_sectors > g_fat_cap_sectors) {
        const uint64_t pages = (static_cast<uint64_t>(need_sectors) * 512 + 4095) / 4096;
        const uint64_t phys = pmm_alloc_contiguous(pages);
        if (!phys || phys >= 0x100000000ULL) return false;
        g_fat = reinterpret_cast<uint32_t*>(phys);
        g_fat_cap_sectors = static_cast<uint32_t>(pages * 8);
    }
    g_dev = dev;
    g_sb = sb;
    g_fat_entries = sb.total_clusters;
    if (!load_fat()) {
        g_dev = nullptr;
        return false;
    }

    // --- mount-time integrity (fsck-lite) ---
    // 1) Superblock field sanity already checked above.
    // 2) Recount free clusters from the FAT; correct silent drift.
    // 3) Clear illegal FAT entries that point outside the volume.
    {
        uint32_t free_n = 0;
        for (uint32_t c = 0; c < g_fat_entries && c < g_sb.total_clusters; ++c) {
            uint32_t v = g_fat[c];
            if (c < g_sb.data_start) {
                // Reserved / FAT region — leave as-is unless clearly nonsense
                continue;
            }
            if (v == FAT_FREE) {
                ++free_n;
                continue;
            }
            if (v == FAT_EOF || v == FAT_BAD)
                continue;
            // next-cluster pointer must stay in-range
            if (v >= g_sb.total_clusters || v >= g_fat_entries) {
                // Break the bad link so chains cannot walk off the disk
                fat_set(c, FAT_EOF);
            }
        }
        if (free_n != g_sb.free_clusters) {
            g_sb.free_clusters = free_n;
            flush_sb();
        }
        if (g_fat_dirty)
            flush_fat();
    }

    g_cwd_cluster = sb.root_cluster;
    g_path_depth  = 0;
    g_mounted = true;
    g_fat_dirty = false;
    g_alloc_hint = g_sb.data_start;
    return true;
}

bool sync() {
    if (!g_mounted) return false;
    bool ok = flush_fat() && flush_sb();
    if (ok) ok = aos::storage::flush_blocks(g_dev);      // NVMe: push the write cache to flash
    return ok;
}

// Create one directory entry `name` under `parent`. Returns first cluster via *out_dc.
Result mkdir_one(uint32_t parent, const char* name, uint32_t* out_dc) {
    if (!name_ok(name)) return Result::BadName;
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(parent, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    if (find_ent(ents, n, name) >= 0) return Result::Exists;
    int slot = find_free_slot(parent, ents, n, MAX_DIR_ENTS);
    if (slot < 0) return Result::Full;
    uint32_t dc = alloc_cluster();
    if (!dc) return Result::Full;

    DirEnt seed[DIR_ENTS_PER_CLUSTER]{};
    name_copy(seed[0].name, ".", sizeof(seed[0].name));
    seed[0].type = 2; seed[0].first = dc;
    name_copy(seed[1].name, "..", sizeof(seed[1].name));
    seed[1].type = 2; seed[1].first = parent;
    if (!write_cluster(dc, seed)) { free_chain(dc); return Result::IoError; }

    DirEnt e{};
    name_copy(e.name, name, sizeof(e.name));
    e.type = 2; e.size = 0; e.first = dc;
    if (!write_ent_back(parent, slot, e)) { free_chain(dc); return Result::IoError; }
    if (out_dc) *out_dc = dc;
    return Result::Ok;
}

// mkdir -p semantics: create every missing component along `path`.
Result mkdir(const char* path) {
    if (!g_mounted) return Result::NoDisk;
    if (!path || !*path) return Result::BadName;

    char buf[192];
    size_t plen = 0;
    while (path[plen] && plen + 1 < sizeof(buf)) { buf[plen] = path[plen]; ++plen; }
    if (path[plen]) return Result::BadName;
    buf[plen] = 0;

    uint32_t cur = g_sb.root_cluster;
    size_t i = 0;
    if (buf[0] == '/') {
        i = 1;
        cur = g_sb.root_cluster;
    } else {
        cur = g_cwd_cluster ? g_cwd_cluster : g_sb.root_cluster;
    }

    bool created_any = false;
    while (buf[i]) {
        while (buf[i] == '/') ++i;
        if (!buf[i]) break;
        char comp[22];
        size_t cl = 0;
        while (buf[i] && buf[i] != '/') {
            if (cl + 1 >= sizeof(comp)) return Result::BadName;
            comp[cl++] = buf[i++];
        }
        comp[cl] = 0;

        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            // Best-effort: cannot walk .. without stack; reject relative .. in mkdir -p
            return Result::BadName;
        }
        if (!name_ok(comp)) return Result::BadName;

        uint32_t first = 0; uint8_t ty = 0;
        Result lr = lookup_one(cur, comp, &first, &ty);
        if (lr == Result::Ok) {
            if (ty != 2 || !first) return Result::NotDir;
            cur = first;
            continue;
        }
        if (lr != Result::NotFound) return lr;

        uint32_t dc = 0;
        Result mr = mkdir_one(cur, comp, &dc);
        if (mr != Result::Ok) return mr;
        created_any = true;
        cur = dc;
    }
    if (!created_any) {
        // Path existed entirely as directories
        return Result::Exists;
    }
    sync();
    return Result::Ok;
}

Result create(const char* path) {
    if (!g_mounted) return Result::NoDisk;
    Resolved R{};
    Result rr = resolve_path(path, true, &R);
    if (rr != Result::Ok) return rr;
    if (!R.leaf[0] || R.is_root) return Result::BadName;
    if (R.target_type == 2) return Result::IsDir;
    if (R.target_type == 1) return Result::Exists;

    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int slot = find_free_slot(R.dir_cluster, ents, n, MAX_DIR_ENTS);
    if (slot < 0) return Result::Full;
    DirEnt e{};
    name_copy(e.name, R.leaf, sizeof(e.name));
    e.type  = 1;
    e.size  = 0;
    e.first = 0;
    if (!write_ent_back(R.dir_cluster, slot, e)) return Result::IoError;
    sync();
    return Result::Ok;
}

Result write(const char* path, const void* data, size_t len) {
    if (!g_mounted) return Result::NoDisk;
    if (len > CONTENT_MAX) return Result::TooBig;
    Resolved R{};
    Result rr = resolve_path(path, true, &R);
    if (rr != Result::Ok) return rr;
    if (!R.leaf[0] || R.is_root) return Result::BadName;
    if (R.target_type == 2) return Result::IsDir;

    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int idx = find_ent(ents, n, R.leaf);
    if (idx < 0) {
        Result r = create(path);
        if (r != Result::Ok) return r;
        n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
        if (n < 0) return Result::IoError;
        idx = find_ent(ents, n, R.leaf);
        if (idx < 0) return Result::IoError;
    }
    // Crash-safe overwrite: write a NEW chain first, then swing the dirent,
    // then free the old chain. A crash mid-way leaves either old or new data
    // reachable — never a dirent pointing at freed clusters.
    const uint32_t old_first = ents[idx].first;
    uint32_t neu = 0;
    if (len) {
        neu = write_file_data(&neu, static_cast<const uint8_t*>(data), len);
        if (!neu) return Result::Full;
        // Push FAT so the new chain is durable before the dirent swings.
        if (!flush_fat()) {
            free_chain(neu);
            return Result::IoError;
        }
    }
    ents[idx].first = neu;
    ents[idx].size  = static_cast<uint32_t>(len);
    ents[idx].type  = 1;
    if (!write_ent_back(R.dir_cluster, idx, ents[idx])) {
        if (neu) free_chain(neu);
        return Result::IoError;
    }
    if (old_first) free_chain(old_first);
    sync();
    return Result::Ok;
}

Result append(const char* path, const void* data, size_t len) {
    if (!g_mounted) return Result::NoDisk;
    if (!data && len) return Result::BadName;
    Resolved R{};
    Result rr = resolve_path(path, true, &R);
    if (rr != Result::Ok) return rr;
    if (!R.leaf[0]) return Result::BadName;
    if (R.target_type == 2) return Result::IsDir;
    if (R.target_type == 0) {
        return write(path, data, len);
    }
    // Read existing limited content path: use chain append via write_file_data on new total
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int idx = find_ent(ents, n, R.leaf);
    if (idx < 0) return Result::NotFound;
    if (ents[idx].type != 1) return Result::IsDir;

    const size_t old_sz = ents[idx].size;
    if (old_sz + len > CONTENT_MAX) return Result::TooBig;
    // Load old into temp buffer (cap CONTENT_MAX). Fail on short read so we never
    // silently truncate a file when a cluster read fails mid-chain.
    uint8_t* tmp = g_read_node.content; // reuse static buffer carefully (held under FS_LOCK)
    size_t got = 0;
    if (old_sz) {
        const size_t want = old_sz > CONTENT_MAX ? CONTENT_MAX : old_sz;
        got = read_file_data(ents[idx].first, ents[idx].size, tmp, want);
        if (got != want) return Result::IoError;
    }
    if (got + len > CONTENT_MAX) return Result::TooBig;
    if (len && data) {
        const uint8_t* src = static_cast<const uint8_t*>(data);
        for (size_t k = 0; k < len; ++k) tmp[got + k] = src[k];
    }
    const size_t total = got + len;
    const uint32_t old_first = ents[idx].first;
    uint32_t neu = 0;
    if (total) {
        neu = write_file_data(&neu, tmp, total);
        if (!neu) return Result::Full;
        if (!flush_fat()) {
            free_chain(neu);
            return Result::IoError;
        }
    }
    ents[idx].first = neu;
    ents[idx].size = static_cast<uint32_t>(total);
    if (!write_ent_back(R.dir_cluster, idx, ents[idx])) {
        if (neu) free_chain(neu);
        return Result::IoError;
    }
    if (old_first) free_chain(old_first);
    sync();
    return Result::Ok;
}

Result remove(const char* path) {
    if (!g_mounted) return Result::NoDisk;
    Resolved R{};
    Result rr = resolve_path(path, true, &R);
    if (rr != Result::Ok) return rr;
    if (!R.leaf[0] || R.is_root) return Result::BadName;
    if (is_dot_or_dotdot(R.leaf)) return Result::BadName;
    if (R.target_type == 0) return Result::NotFound;

    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int idx = find_ent(ents, n, R.leaf);
    if (idx < 0) return Result::NotFound;

    const uint32_t first = ents[idx].first;
    const uint8_t  type  = ents[idx].type;

    if (type == 2) {
        DirEnt sub[MAX_DIR_ENTS]{};
        int sn = load_dir(first, sub, MAX_DIR_ENTS);
        if (sn < 0) return Result::IoError;
        for (int i = 0; i < sn; ++i) {
            if (sub[i].type != 0 && !is_dot_or_dotdot(sub[i].name))
                return Result::NotEmpty;
        }
        // Refuse removing a directory we are currently inside
        if (first == g_cwd_cluster) return Result::BadName;
        for (int k = 0; k < g_path_depth; ++k)
            if (g_path[k].cluster == first) return Result::BadName;
    }

    DirEnt blank{};
    if (!write_ent_back(R.dir_cluster, idx, blank)) return Result::IoError;
    if (first) free_chain(first);
    sync();
    return Result::Ok;
}

Result rename(const char* old_path, const char* new_path) {
    if (!g_mounted) return Result::NoDisk;
    Resolved O{}, N{};
    Result r1 = resolve_path(old_path, true, &O);
    if (r1 != Result::Ok) return r1;
    Result r2 = resolve_path(new_path, true, &N);
    if (r2 != Result::Ok) return r2;
    if (!O.leaf[0] || !N.leaf[0]) return Result::BadName;
    if (is_dot_or_dotdot(O.leaf) || is_dot_or_dotdot(N.leaf)) return Result::BadName;
    if (O.target_type == 0) return Result::NotFound;
    if (N.target_type != 0) return Result::Exists;

    // Refuse moving a directory that contains cwd
    if (O.target_type == 2) {
        if (O.target_cluster == g_cwd_cluster) return Result::BadName;
        for (int k = 0; k < g_path_depth; ++k)
            if (g_path[k].cluster == O.target_cluster) return Result::BadName;
    }

    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(O.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return Result::IoError;
    int idx = find_ent(ents, n, O.leaf);
    if (idx < 0) return Result::NotFound;
    DirEnt moved = ents[idx];

    if (O.dir_cluster == N.dir_cluster) {
        // Same directory: rename in place
        name_copy(moved.name, N.leaf, sizeof(moved.name));
        if (!write_ent_back(O.dir_cluster, idx, moved)) return Result::IoError;
        sync();
        return Result::Ok;
    }

    // Cross-directory move: insert into destination first, then clear source
    DirEnt dents[MAX_DIR_ENTS]{};
    int dn = load_dir(N.dir_cluster, dents, MAX_DIR_ENTS);
    if (dn < 0) return Result::IoError;
    int slot = find_free_slot(N.dir_cluster, dents, dn, MAX_DIR_ENTS);
    if (slot < 0) return Result::Full;

    name_copy(moved.name, N.leaf, sizeof(moved.name));
    if (!write_ent_back(N.dir_cluster, slot, moved)) return Result::IoError;

    // If directory, fix ".." to new parent
    if (moved.type == 2 && moved.first) {
        DirEnt sub[MAX_DIR_ENTS]{};
        int sn = load_dir(moved.first, sub, MAX_DIR_ENTS);
        if (sn >= 0) {
            for (int i = 0; i < sn; ++i) {
                if (sub[i].type == 2 && sub[i].name[0]=='.' && sub[i].name[1]=='.' && sub[i].name[2]==0) {
                    sub[i].first = N.dir_cluster;
                    write_ent_back(moved.first, i, sub[i]);
                    break;
                }
            }
        }
    }

    DirEnt blank{};
    if (!write_ent_back(O.dir_cluster, idx, blank)) {
        // Best-effort rollback of destination entry
        write_ent_back(N.dir_cluster, slot, blank);
        return Result::IoError;
    }
    sync();
    return Result::Ok;
}

Result cd(const char* path) {
    if (!g_mounted) return Result::NoDisk;
    if (!path || !*path) return Result::BadName;
    return apply_cd_to_stack(path);
}

const Node* read(const char* path) {
    if (!g_mounted || !path) return nullptr;
    Resolved R{};
    if (resolve_path(path, true, &R) != Result::Ok) return nullptr;
    if (!R.leaf[0] || R.target_type != 1) return nullptr;
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return nullptr;
    int idx = find_ent(ents, n, R.leaf);
    if (idx < 0 || ents[idx].type != 1) return nullptr;
    g_read_node = {};
    name_copy(g_read_node.name, ents[idx].name, sizeof(g_read_node.name));
    g_read_node.used = true;
    g_read_node.is_dir = false;
    g_read_node.parent = -1;
    size_t want = ents[idx].size;
    if (want > CONTENT_MAX) want = CONTENT_MAX;
    g_read_node.size = read_file_data(ents[idx].first, ents[idx].size,
                                      g_read_node.content, want);
    return &g_read_node;
}

void cwd_path(char* buf, size_t buflen) {
    if (!buf || !buflen) return;
    if (!g_mounted) { buf[0] = 0; return; }

    if (g_path_depth <= 0) {
        buf[0] = '/';
        if (buflen > 1) buf[1] = 0;
        return;
    }

    // Build "/a/b/c" from the path stack.
    size_t pos = 0;
    for (int i = 0; i < g_path_depth; ++i) {
        if (pos + 1 >= buflen) break;
        buf[pos++] = '/';
        const char* n = g_path[i].name;
        while (*n && pos + 1 < buflen) buf[pos++] = *n++;
    }
    buf[pos < buflen ? pos : buflen - 1] = 0;
}

size_t node_count() {
    if (!g_mounted) return 0;
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(g_cwd_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return 0;
    size_t c = 0;
    for (int i = 0; i < n; ++i)
        if (ents[i].type != 0 && !is_dot_or_dotdot(ents[i].name))
            ++c;
    return c;
}

void list(void (*cb)(const char*, bool, size_t, void*), void* ctx) {
    if (!g_mounted || !cb) return;
    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(g_cwd_cluster, ents, MAX_DIR_ENTS);
    if (n < 0) return;
    for (int i = 0; i < n; ++i)
        if (ents[i].type != 0 && !is_dot_or_dotdot(ents[i].name))
            cb(ents[i].name, ents[i].type == 2, ents[i].size, ctx);
}


void list_path(const char* path, void (*cb)(const char*, bool, size_t, void*), void* ctx) {
    if (!g_mounted || !cb) return;
    if (!path || !*path || (path[0]=='.' && path[1]==0)) {
        list(cb, ctx);
        return;
    }
    if (path[0]=='/' && path[1]==0) {
        DirEnt ents[MAX_DIR_ENTS]{};
        int n = load_dir(g_sb.root_cluster, ents, MAX_DIR_ENTS);
        if (n < 0) return;
        for (int i = 0; i < n; ++i)
            if (ents[i].type != 0 && !is_dot_or_dotdot(ents[i].name))
                cb(ents[i].name, ents[i].type == 2, ents[i].size, ctx);
        return;
    }

    Resolved R{};
    Result rr = resolve_path(path, true, &R);
    if (rr != Result::Ok) return;

    uint32_t dir = 0;
    if (R.target_type == 2 && R.target_cluster) {
        dir = R.target_cluster;
    } else if (R.target_type == 1) {
        DirEnt ents[MAX_DIR_ENTS]{};
        int n = load_dir(R.dir_cluster, ents, MAX_DIR_ENTS);
        if (n < 0) return;
        int idx = find_ent(ents, n, R.leaf);
        if (idx >= 0)
            cb(ents[idx].name, false, ents[idx].size, ctx);
        return;
    } else {
        return; // not found
    }

    DirEnt ents[MAX_DIR_ENTS]{};
    int n = load_dir(dir, ents, MAX_DIR_ENTS);
    if (n < 0) return;
    for (int i = 0; i < n; ++i)
        if (ents[i].type != 0 && !is_dot_or_dotdot(ents[i].name))
            cb(ents[i].name, ents[i].type == 2, ents[i].size, ctx);
}

Result copy(const char* src, const char* dst) {
    if (!g_mounted) return Result::NoDisk;
    const Node* n = read(src);
    if (!n) return Result::NotFound;
    return write(dst, n->content, n->size);
}

}  // namespace aos::aosfs
