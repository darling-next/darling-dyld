/* perf#24c2c — DCC2 cache reader (flag-gated). Loads region-packed images from a DCC2 cache and
 * applies the cache-native fixup table, bypassing the normal Mach-O fixup engine (which is
 * known-wrong for region-packed layout — perf#24c2a).
 *
 * Feature-gated ONLY by env:
 *   DARLING_DYLD_DCC2=1
 *   DARLING_DYLD_DCC2_PATH=/path/to/cache.dcc2
 * Env absent  => reader is a no-op; dyld behaves exactly as before.
 * Cache invalid/stale => clean fallback (reader disables itself) OR explicit hard fail; never a
 *   half-cache / half-normal state.
 */
#ifndef __DYLD_DCC2_READER_H__
#define __DYLD_DCC2_READER_H__

#include <stdint.h>
#include "MachOLoaded.h"

namespace dyld3 {

/* on-disk format (must match tools/closure-cache/dcc2-format.h AND dcc5-format.h).
 * perf#24f: DCC5 is binary-compatible with DCC2 (same regions + fixup table) EXCEPT (a) magic/version,
 * (b) the header carries 4 extra riprel_* accounting uint32 at the end, and (c) the packed __TEXT has
 * its RIP-relative disp32 pre-rewritten so region-packing no longer breaks TEXT->DATA refs (the DCC2
 * "__text" crash, task #97). The reader accepts BOTH: for DCC2 it maps stale __TEXT (crashes on any
 * non-leaf image — kept only for the historical 2-dylib smoke); for DCC5 the __TEXT is correct.
 * The header struct below matches dcc5-format.h so sizeof() places the image table correctly for a
 * DCC5 cache; a DCC2 cache written by the old builder has a smaller header, so DCC2 is now read via the
 * legacy path only (magic gate) — production uses DCC5. */
#define DCC2_MAGIC   0x44434332u
#define DCC2_VERSION 2
#define DCC5_MAGIC   0x44434335u
#define DCC5_VERSION 5
/* perf#24f-#107: DCC6 adds per-seg orig_vmaddr/orig_vmsize (dcc_seg grew) so exported-symbol
 * addresses (original-image-relative trie offsets) translate correctly to the rewritten arena. */
#define DCC6_MAGIC   0x44434336u
#define DCC6_VERSION 6
#define DCC2_REGION_ALIGN 0x4000
#define DCC2_MAX_SEGS 8
#define DCC2_FIX_REBASE        0
#define DCC2_FIX_BIND_INTERNAL 1
#define DCC2_FIX_BIND_EXTERN   2
#define DCC2_FIX_BIND_EXTERN_LAZY 3  /* lazy bind: resolve if possible, else write sentinel (no hard-fail) */

struct DCC2Region { uint64_t file_off, size, vm_base; uint32_t prot, _pad; };
/* perf#24f-#107: orig_vmaddr/orig_vmsize are the ORIGINAL (pre-repack) seg vmaddr/vmsize; vmaddr/vmsize
 * are the REWRITTEN region-relative values. MUST match tools/closure-cache/dcc5-format.h (DCC6). */
struct DCC2Seg    { char name[16]; uint64_t vmaddr, vmsize, region_off, filesize; uint32_t region_idx, prot; uint64_t orig_vmaddr, orig_vmsize; };
struct DCC2Image {
    char     path[256];
    uint8_t  uuid[16];
    uint64_t src_inode, src_mtime, src_size;
    uint64_t image_vmbase;
    uint32_t nsegs, init_index;
    uint32_t fixup_first, fixup_count;
    DCC2Seg  segs[DCC2_MAX_SEGS];
};
struct DCC2Fixup {
    uint8_t  kind, loc_region, tgt_region, _pad;
    uint32_t image_index;
    uint64_t loc_off, tgt_off;
    int64_t  addend;
    uint32_t extern_sym, _pad2;
};
struct DCC2Header {
    uint32_t magic, version, arch, image_count;
    uint64_t closure_hash;
    char     install_root[256];
    DCC2Region regions[3];
    uint64_t fixup_off;  uint32_t fixup_count;  uint32_t _pad0;
    uint64_t extern_off; uint32_t extern_size;  uint32_t _pad1;
    /* perf#24f DCC5 rewrite totals (present only in DCC5; included so sizeof() matches the DCC5 header
     * and the image table starts at the right offset). */
    uint32_t riprel_total, riprel_rewritten, riprel_sametext, riprel_skipped_pool;
};

//
// Singleton reader. Initialized once (flag-gated) at the top of mapAndFixupAllImages.
//
class DCC2Reader {
public:
    typedef bool (*LogFunc)(const char*, ...) __attribute__((format(printf, 1, 2)));

    // Returns the enabled reader if DARLING_DYLD_DCC2=1 and the cache mapped+validated OK.
    // Returns nullptr if the flag is absent (normal dyld). If the flag is present but the cache is
    // invalid: on hardFail it halts; otherwise returns nullptr (clean fallback to normal dyld).
    static DCC2Reader*  init(const char* envp[], LogFunc log);

    // perf#24c2e: process-wide singleton for the dyld2 classic path. initShared() is called once,
    // early in dyld2 _main; shared() returns it (or nullptr if the flag was absent / cache invalid).
    // This lets ImageLoaderMachO* reach the reader without threading a pointer through the loader.
    static void         initShared(const char* envp[], LogFunc log);
    static DCC2Reader*  shared();

    bool                enabled() const { return _arena != 0; }

    // If 'path' is a DCC2-cached image, returns its runtime loaded address (arena + image_vmbase)
    // and marks it DCC-owned; else returns nullptr (image loads normally).
    const MachOLoaded*  loadedAddressFor(const char* path, uint32_t* outImageIndex);

    // True if the image at this loadedAddress is DCC-owned (used to enforce the invariant).
    bool                isDCCImage(const MachOLoaded* mh) const;

    // perf#24c2e (dyld2 classic path): cheap+exact test used by the guarded hook in
    // ImageLoaderMachOCompressed::doRebase/doBind. A mach_header* is DCC-owned iff it points inside
    // the RX region at one of the registered image_vmbase offsets. Also returns the image index.
    bool                isDCC2Image(const struct mach_header* mh, uint32_t* outIndex = nullptr) const;

    // perf#24c2e: apply ALL cached images' fixups exactly once for the whole cache (bind targets are
    // cross-image, so this must run once after all DCC2 images are registered, not per-image).
    // resolveExtern resolves a flat/extern symbol; a required symbol returning found=false is a hard
    // fail. Returns false on any hard-fail condition. Idempotent-guarded: a second call is a no-op
    // that returns true (already applied).
    bool                applyAllFixupsOnce(LogFunc logFixups,
                                           uintptr_t (^resolveExtern)(const char* symbolName, bool& found));
    bool                allFixupsApplied() const { return _allApplied; }

    // perf#24c2e counters (for the smoke gate; logged via dumpCounters()).
    void                noteNormalRebaseSkipped() { ++_cNormalRebaseSkipped; }
    void                noteNormalBindSkipped()   { ++_cNormalBindSkipped; }
    void                noteImageRegistered()     { ++_cImagesRegistered; }
    void                dumpCounters(LogFunc log) const;

    // Apply this image's cache-native fixups to the (COW) RW region. 'resolveExtern' resolves a
    // flat/extern symbol name to a runtime address (the normal dyld symbol resolver); if it returns
    // 0 for a required symbol, this is a hard fail (no silent NULL).
    // Returns false on any hard-fail condition.
    bool                applyFixups(uint32_t imageIndex, LogFunc logFixups,
                                    uintptr_t (^resolveExtern)(const char* symbolName, bool& found));

    uint64_t            slide() const { return _arena; }
    const char*         cachePath() const { return _path; }

    // perf#24f-fix-flockfile-datafixup (#105b): translate an image-relative vmaddr (as stored in the
    // export trie / LC_SEGMENT vmaddrs of the ORIGINAL image) to its runtime address in the 3-region
    // arena. Normal dyld computes exportAddr = fMachOData + trieOffset, which is only correct for a
    // contiguously-mapped image; for a region-packed DCC image a __DATA export (e.g. ___stdinp) would
    // land inside the RX/__TEXT region. This walks the per-image seg table to find the region+offset.
    // Returns 0 if imageIndex is invalid or vmaddr is not covered by any of the image's segments.
    uintptr_t           translateVmaddr(uint32_t imageIndex, uint64_t imageRelVmaddr) const;

private:
    bool                mapRegions(int fd);
    bool                validate(int fd);

    uint8_t*            _cache      = nullptr;   // mmap of the cache file (header + tables)
    uint64_t            _cacheSize  = 0;
    uint64_t            _arena      = 0;         // base of the 3-region arena (== uniform slide, RX.vm_base==0)
    uint8_t*            _rmap[3]    = { nullptr, nullptr, nullptr };
    int                 _fd         = -1;
    char                _path[512]  = { 0 };
    LogFunc             _log        = nullptr;

    DCC2Header*         _hdr        = nullptr;
    DCC2Image*          _images     = nullptr;
    DCC2Fixup*          _fixups     = nullptr;
    const char*         _externStr  = nullptr;

    // record which loadedAddresses are DCC-owned (small fixed set for the smoke)
    static const int    kMaxDCC = 128;
    const MachOLoaded*  _dccAddrs[kMaxDCC] = { nullptr };
    int                 _nDccAddrs = 0;
    bool                _appliedOnce[kMaxDCC] = { false };  // enforce "applied exactly once" (per-image path)

    // perf#24c2e: whole-cache single-apply guard + counters for the dyld2 classic path
    bool                _allApplied = false;
    uint32_t            _cImagesRegistered   = 0;
    uint32_t            _cNormalRebaseSkipped = 0;
    uint32_t            _cNormalBindSkipped   = 0;
    uint32_t            _cFixupsApplied       = 0;
};

} // namespace dyld3

#endif
