/* perf#24c2c — DCC2 cache reader implementation (flag-gated). See DCC2Reader.h. */

#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <System/sys/mman.h>
#include <mach/mach.h>
#include <_simple.h>

#include "DCC2Reader.h"
#include "MachOFile.h"
#include "dyld2.h"

namespace dyld3 {

// dyld3::open / dyld3::stat live in MachOFile.cpp
int  open(const char* path, int flag, int other);
int  stat(const char* path, struct stat* buf);

static uint64_t roundup2(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

DCC2Reader* DCC2Reader::init(const char* envp[], LogFunc log)
{
    const char* on = _simple_getenv(envp, "DARLING_DYLD_DCC2");
    if ( (on == nullptr) || (on[0] != '1') )
        return nullptr;                       // flag absent => exact old behavior

    const char* path = _simple_getenv(envp, "DARLING_DYLD_DCC2_PATH");
    bool hardFail = true;                     // present flag but broken cache: default to hard fail
    const char* soft = _simple_getenv(envp, "DARLING_DYLD_DCC2_SOFT");
    if ( soft && soft[0]=='1' ) hardFail = false;  // opt-in clean fallback for experiments

    static DCC2Reader reader;                 // singleton (dyld is single-threaded at this point)
    reader._log = log;

    if ( path == nullptr ) {
        if ( log ) log("dyld[DCC2]: DARLING_DYLD_DCC2=1 but DARLING_DYLD_DCC2_PATH unset\n");
        if ( hardFail ) dyld::halt("DCC2: cache path unset");
        return nullptr;
    }
    strlcpy(reader._path, path, sizeof reader._path);

    int fd = dyld3::open(path, O_RDONLY, 0);
    if ( fd < 0 ) {
        if ( log ) log("dyld[DCC2]: open('%s') failed errno=%d\n", path, errno);
        if ( hardFail ) dyld::halt("DCC2: cannot open cache");
        return nullptr;
    }
    if ( !reader.validate(fd) || !reader.mapRegions(fd) ) {
        ::close(fd);
        reader._arena = 0;
        if ( hardFail ) dyld::halt("DCC2: cache invalid/stale");
        return nullptr;
    }
    reader._fd = fd;
    if ( log ) log("dyld[DCC2]: enabled, %u images from '%s' (arena=0x%llx)\n",
                   reader._hdr->image_count, path, (unsigned long long)reader._arena);
    return &reader;
}

// perf#24c2e: process-wide singleton for the dyld2 classic path.
static DCC2Reader* sSharedDCC2 = nullptr;

void DCC2Reader::initShared(const char* envp[], LogFunc log)
{
    sSharedDCC2 = DCC2Reader::init(envp, log);   // nullptr if flag absent / cache invalid (soft)
}

DCC2Reader* DCC2Reader::shared()
{
    return sSharedDCC2;
}

bool DCC2Reader::validate(int fd)
{
    struct stat st;
    if ( fstat(fd, &st) != 0 ) return false;
    _cacheSize = st.st_size;
    void* m = ::mmap(nullptr, _cacheSize, PROT_READ, MAP_PRIVATE, fd, 0);
    if ( m == MAP_FAILED ) return false;
    _cache = (uint8_t*)m;
    _hdr   = (DCC2Header*)_cache;
    if ( _hdr->magic != DCC2_MAGIC )   { if(_log)_log("dyld[DCC2]: bad magic\n"); return false; }
    if ( _hdr->version != DCC2_VERSION){ if(_log)_log("dyld[DCC2]: bad version\n"); return false; }
    _images    = (DCC2Image*)(_cache + sizeof(DCC2Header));
    _fixups    = (DCC2Fixup*)(_cache + _hdr->fixup_off);
    _externStr = (const char*)(_cache + _hdr->extern_off);

    // staleness: every source dylib's inode/mtime/size must match what the builder recorded.
    for ( uint32_t i = 0; i < _hdr->image_count; ++i ) {
        DCC2Image* im = &_images[i];
        struct stat s2;
        if ( dyld3::stat(im->path, &s2) != 0 ) {
            if(_log)_log("dyld[DCC2]: stale/missing source '%s'\n", im->path); return false;
        }
        if ( (uint64_t)s2.st_ino != im->src_inode || (uint64_t)s2.st_mtime != im->src_mtime || (uint64_t)s2.st_size != im->src_size ) {
            if(_log)_log("dyld[DCC2]: stale source '%s' (inode/mtime/size changed)\n", im->path); return false;
        }
    }
    return true;
}

bool DCC2Reader::mapRegions(int fd)
{
    // reserve one arena covering [0 .. max(vm_base+size)] then MAP_FIXED each region so relative
    // spacing is preserved (one uniform slide). RX+RO shared, RW private (COW).
    uint64_t span = 0;
    for (int i=0;i<3;i++){ uint64_t end=_hdr->regions[i].vm_base + roundup2(_hdr->regions[i].size, DCC2_REGION_ALIGN); if(end>span)span=end; }
    vm_address_t arena = 0;
    if ( ::vm_allocate(mach_task_self(), &arena, (vm_size_t)span, VM_FLAGS_ANYWHERE) != KERN_SUCCESS ) {
        if(_log)_log("dyld[DCC2]: vm_allocate(0x%llx) failed\n", (unsigned long long)span); return false;
    }
    _arena = (uint64_t)arena;
    int prot[3] = { PROT_READ|PROT_EXEC, PROT_READ|PROT_WRITE, PROT_READ };
    int flags[3]= { MAP_FIXED|MAP_PRIVATE, MAP_FIXED|MAP_PRIVATE /*COW*/, MAP_FIXED|MAP_PRIVATE };
    for (int i=0;i<3;i++){
        void* want = (void*)(_arena + _hdr->regions[i].vm_base);
        void* got  = ::mmap(want, roundup2(_hdr->regions[i].size, DCC2_REGION_ALIGN), prot[i], flags[i], fd, _hdr->regions[i].file_off);
        if ( got == MAP_FAILED ) { if(_log)_log("dyld[DCC2]: map region %d failed errno=%d\n", i, errno); return false; }
        _rmap[i] = (uint8_t*)got;
    }
    // one-slide invariant: relative spacing preserved
    if ( ((uint64_t)_rmap[1] - _hdr->regions[1].vm_base != _arena) ||
         ((uint64_t)_rmap[2] - _hdr->regions[2].vm_base != _arena) ) {
        if(_log)_log("dyld[DCC2]: region spacing not preserved (one-slide broken)\n"); return false;
    }
    return true;
}

const MachOLoaded* DCC2Reader::loadedAddressFor(const char* path, uint32_t* outImageIndex)
{
    if ( !enabled() ) return nullptr;
    for ( uint32_t i = 0; i < _hdr->image_count; ++i ) {
        if ( strcmp(_images[i].path, path) == 0 ) {
            const MachOLoaded* mh = (const MachOLoaded*)(_arena + _images[i].image_vmbase);
            if ( _nDccAddrs < kMaxDCC ) _dccAddrs[_nDccAddrs++] = mh;
            if ( outImageIndex ) *outImageIndex = i;
            return mh;
        }
    }
    return nullptr;
}

bool DCC2Reader::isDCCImage(const MachOLoaded* mh) const
{
    for ( int i = 0; i < _nDccAddrs; ++i ) if ( _dccAddrs[i] == mh ) return true;
    return false;
}

// perf#24c2e: cheap+exact — a mach_header is DCC-owned iff it sits at _arena + image_vmbase for one
// of the cached images. (Exact pointer identity, not a range test.)
bool DCC2Reader::isDCC2Image(const struct mach_header* mh, uint32_t* outIndex) const
{
    if ( !enabled() || mh == nullptr ) return false;
    const uint64_t addr = (uint64_t)mh;
    for ( uint32_t i = 0; i < _hdr->image_count; ++i ) {
        if ( addr == (_arena + _images[i].image_vmbase) ) {
            if ( outIndex ) *outIndex = i;
            return true;
        }
    }
    return false;
}

// perf#24c2e: apply EVERY cached image's fixups, once, for the whole cache. Bind targets are
// cross-image (BIND_INTERNAL references another cached image's region), so this must run after all
// DCC2 images are registered — hence once-per-cache, not per-image. Idempotent: a second call is a
// no-op returning true.
bool DCC2Reader::applyAllFixupsOnce(LogFunc logFixups,
                                    uintptr_t (^resolveExtern)(const char* symbolName, bool& found))
{
    if ( !enabled() ) return false;
    if ( _allApplied ) return true;                  // already applied for this cache
    const uint64_t slide = _arena;
    uint32_t applied = 0;
    for ( uint32_t fi = 0; fi < _hdr->fixup_count; ++fi ) {
        DCC2Fixup* f = &_fixups[fi];
        if ( f->loc_region != 1 ) { if(_log)_log("dyld[DCC2]: fixup %u loc not in RW (region=%d) hard fail\n", fi, f->loc_region); return false; }
        uintptr_t* loc = (uintptr_t*)(_rmap[1] + f->loc_off);
        switch ( f->kind ) {
            case DCC2_FIX_REBASE:
                *loc += (uintptr_t)slide;
                break;
            case DCC2_FIX_BIND_INTERNAL:
                *loc = (uintptr_t)(_hdr->regions[f->tgt_region].vm_base + f->tgt_off + slide + f->addend);
                break;
            case DCC2_FIX_BIND_EXTERN: {
                const char* sym = _externStr + f->extern_sym;
                bool found = false;
                uintptr_t v = resolveExtern ? resolveExtern(sym, found) : 0;
                if ( !found ) { if(_log)_log("dyld[DCC2]: extern symbol '%s' unresolved (hard fail)\n", sym); return false; }
                *loc = v + (uintptr_t)f->addend;
                break;
            }
            case DCC2_FIX_BIND_EXTERN_LAZY: {
                // A lazy bind whose symbol may be genuinely absent (Darling stub gap). Try to resolve; if
                // not found, write a sentinel (0) instead of hard-failing — preserving lazy "resolve/trap
                // on first call" semantics for a symbol that is not called on the paths we exercise.
                const char* sym = _externStr + f->extern_sym;
                bool found = false;
                uintptr_t v = resolveExtern ? resolveExtern(sym, found) : 0;
                *loc = found ? (v + (uintptr_t)f->addend) : (uintptr_t)0;
                if ( !found && _log ) _log("dyld[DCC2]: lazy extern '%s' unresolved => sentinel 0 (never-called ok)\n", sym);
                break;
            }
            default:
                if(_log)_log("dyld[DCC2]: unknown fixup kind %d (hard fail)\n", f->kind); return false;
        }
        ++applied;
        if ( logFixups ) logFixups("dyld[DCC2]: fixup #%u kind=%d loc=%p\n", fi, f->kind, (void*)loc);
    }
    _allApplied = true;
    _cFixupsApplied = applied;
    if ( _log ) _log("dyld[DCC2]: applied %u fixups once for whole cache (slide=0x%llx)\n",
                     applied, (unsigned long long)slide);
    return true;
}

void DCC2Reader::dumpCounters(LogFunc log) const
{
    if ( !log ) return;
    log("dyld[DCC2]: counters: reader_init=1 regions_mapped=3 images_registered=%u "
        "normal_rebase_skipped=%u normal_bind_skipped=%u fixups_applied_once=%d fixup_count=%u\n",
        _cImagesRegistered, _cNormalRebaseSkipped, _cNormalBindSkipped,
        _allApplied ? 1 : 0, _cFixupsApplied);
}

bool DCC2Reader::applyFixups(uint32_t imageIndex, LogFunc logFixups,
                             uintptr_t (^resolveExtern)(const char* symbolName, bool& found))
{
    if ( !enabled() || imageIndex >= _hdr->image_count ) return false;
    // applied exactly once
    if ( imageIndex < (uint32_t)kMaxDCC ) {
        if ( _appliedOnce[imageIndex] ) { if(_log)_log("dyld[DCC2]: fixups already applied for image %u\n", imageIndex); return false; }
        _appliedOnce[imageIndex] = true;
    }
    DCC2Image* im = &_images[imageIndex];
    const uint64_t slide = _arena;
    for ( uint32_t k = 0; k < im->fixup_count; ++k ) {
        DCC2Fixup* f = &_fixups[im->fixup_first + k];
        if ( f->image_index != imageIndex ) { if(_log)_log("dyld[DCC2]: fixup image mismatch\n"); return false; }
        if ( f->loc_region != 1 ) { if(_log)_log("dyld[DCC2]: fixup loc not in RW\n"); return false; }
        uintptr_t* loc = (uintptr_t*)(_rmap[1] + f->loc_off);
        switch ( f->kind ) {
            case DCC2_FIX_REBASE:
                *loc += (uintptr_t)slide;
                break;
            case DCC2_FIX_BIND_INTERNAL:
                *loc = (uintptr_t)(_hdr->regions[f->tgt_region].vm_base + f->tgt_off + slide + f->addend);
                break;
            case DCC2_FIX_BIND_EXTERN: {
                const char* sym = _externStr + f->extern_sym;
                bool found = false;
                uintptr_t v = resolveExtern ? resolveExtern(sym, found) : 0;
                if ( !found ) { if(_log)_log("dyld[DCC2]: extern symbol '%s' unresolved (hard fail)\n", sym); return false; }
                *loc = v + (uintptr_t)f->addend;
                break;
            }
            case DCC2_FIX_BIND_EXTERN_LAZY: {
                const char* sym = _externStr + f->extern_sym;
                bool found = false;
                uintptr_t v = resolveExtern ? resolveExtern(sym, found) : 0;
                *loc = found ? (v + (uintptr_t)f->addend) : (uintptr_t)0;
                break;
            }
            default:
                if(_log)_log("dyld[DCC2]: unknown fixup kind %d\n", f->kind); return false;
        }
        if ( logFixups ) logFixups("dyld[DCC2]: fixup %s:%p (kind=%d)\n", im->path, (void*)loc, f->kind);
    }
    return true;
}

} // namespace dyld3
