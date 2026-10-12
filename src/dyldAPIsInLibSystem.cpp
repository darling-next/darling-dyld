/* -*- mode: C++; c-basic-offset: 4; tab-width: 4 -*-
 *
 * Copyright (c) 2004-2012 Apple Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

#include <stddef.h>
#include <string.h>
#include <malloc/malloc.h>
#include <sys/mman.h>
#include <execinfo.h>

#include <TargetConditionals.h>
#include <System/sys/csr.h>
#include <crt_externs.h>
#include <Availability.h>
#if !TARGET_OS_DRIVERKIT
#include <vproc_priv.h>
#endif
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <fcntl.h>
#include <System/sys/codesign.h>
#include <libc_private.h>

#include <mach-o/dyld_images.h>
#include <mach-o/dyld.h>
#include <mach-o/dyld_priv.h>
#include <mach-o/dyld_process_info.h>

#include <ptrauth.h>

#include "dyld_cache_format.h"
#include "objc-shared-cache.h"

#include "ImageLoader.h"
#include "dyldLock.h"

#include "APIs.h"
#include "AllImages.h"
#include "StartGlue.h"
#include "Tracing.h"


// this was in dyld_priv.h but it is no longer exported
extern "C" {
    const struct dyld_all_image_infos* _dyld_get_all_image_infos() __attribute__((visibility("hidden")));
}


extern "C" int  __cxa_atexit(void (*func)(void *), void *arg, void *dso);
extern "C" void __cxa_finalize(const void *dso);
extern "C" void __cxa_finalize_ranges(const struct __cxa_range_t ranges[], int count);

//
// private interface between libSystem.dylib and dyld
//
extern "C" int _dyld_func_lookup(const char* dyld_func_name, void **address);

template<typename T>
static void dyld_func_lookup_and_resign(const char *dyld_func_name, T *__ptrauth_dyld_function_ptr* address) {
    void *funcAsVoidPtr;
    int res = _dyld_func_lookup(dyld_func_name, &funcAsVoidPtr);
    (void)res;

    // If C function pointer discriminators are type-diverse this cast will be
    // an authenticate and resign operation.
    *address = reinterpret_cast<T *>(funcAsVoidPtr);
}

#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
namespace dyld3 {
    extern int compatFuncLookup(const char* name, void** address) __API_AVAILABLE(ios(13.0));
}
extern "C" void setLookupFunc(void*);
#endif


extern void* __ptrauth_dyld_address_auth gUseDyld3;


// <rdar://problem/61161069> libdyld.dylib should use abort_with_payload() for asserts
VIS_HIDDEN
void abort_report_np(const char* format, ...)
{
    va_list list;
    const char *str;
    _SIMPLE_STRING s = _simple_salloc();
    if ( s != NULL ) {
        va_start(list, format);
        _simple_vsprintf(s, format, list);
        va_end(list);
        str = _simple_string(s);
    }
    else {
        // _simple_salloc failed, but at least format may have useful info by itself
        str = format;
    }
    if ( gUseDyld3 ) {
        dyld3::halt(str);
    }
    else {
        typedef void (*funcType)(const char* msg) __attribute__((__noreturn__));
        static funcType __ptrauth_dyld_function_ptr p = NULL;
        dyld_func_lookup_and_resign("__dyld_halt", &p);
        p(str);
    }
    // halt() doesn't return, so we can't call _simple_sfree
}

// libc uses assert()
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winvalid-noreturn"
VIS_HIDDEN
void __assert_rtn(const char* func, const char* file, int line, const char* failedexpr)
{
    if (func == NULL) {
        abort_report_np("Assertion failed: (%s), file %s, line %d.\n", failedexpr, file, line);
    } else {
        abort_report_np("Assertion failed: (%s), function %s, file %s, line %d.\n", failedexpr, func, file, line);
    }
}
#pragma clang diagnostic pop


// deprecated APIs are still availble on Mac OS X, but not on iPhone OS
#if TARGET_OS_OSX
	#define DEPRECATED_APIS_SUPPORTED 1
#else
	#define DEPRECATED_APIS_SUPPORTED 0
#endif

/*
 * names_match() takes an install_name from an LC_LOAD_DYLIB command and a
 * libraryName (which is -lx or -framework Foo argument passed to the static
 * link editor for the same library) and determines if they match.  This depends
 * on conventional use of names including major versioning.
 */
static
bool
names_match(
const char *install_name,
const char* libraryName)
{
    const char *basename;
    unsigned long n;

	/*
	 * Conventional install names have these forms:
	 *	/System/Library/Frameworks/AppKit.framework/Versions/A/Appkit
	 *	/Local/Library/Frameworks/AppKit.framework/Appkit
	 *	/lib/libsys_s.A.dylib
	 *	/usr/lib/libsys_s.dylib
	 */
	basename = strrchr(install_name, '/');
	if(basename == NULL)
	    basename = install_name;
	else
	    basename++;

	/*
	 * By checking the base name matching the library name we take care
	 * of the -framework cases.
	 */
	if(strcmp(basename, libraryName) == 0)
	    return true;

	/*
	 * Now check the base name for "lib" if so proceed to check for the
	 * -lx case dealing with a possible .X.dylib and a .dylib extension.
	 */
	if(strncmp(basename, "lib", 3) ==0){
	    n = strlen(libraryName);
	    if(strncmp(basename+3, libraryName, n) == 0){
		if(strncmp(basename+3+n, ".dylib", 6) == 0)
		    return true;
		if(basename[3+n] == '.' &&
		   basename[3+n+1] != '\0' &&
		   strncmp(basename+3+n+2, ".dylib", 6) == 0)
		    return true;
	    }
	}
	return false;
}

#if DEPRECATED_APIS_SUPPORTED

void NSInstallLinkEditErrorHandlers(
const NSLinkEditErrorHandlers* handlers)
{
	if ( gUseDyld3 )
		return dyld3::NSInstallLinkEditErrorHandlers(handlers);

	DYLD_LOCK_THIS_BLOCK;
	typedef void (*ucallback_t)(const char* symbol_name);
 	typedef NSModule (*mcallback_t)(NSSymbol s, NSModule old, NSModule newhandler);
	typedef void (*lcallback_t)(NSLinkEditErrors c, int errorNumber,
								const char* fileName, const char* errorString);
	typedef void (*funcType)(ucallback_t undefined, mcallback_t multiple, lcallback_t linkEdit);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_install_handlers", &p);
	mcallback_t m = handlers->multiple;
	p(handlers->undefined, m, handlers->linkEdit);
}

const char* 
NSNameOfModule(
NSModule module)
{
	if ( gUseDyld3 )
		return dyld3::NSNameOfModule(module);

	DYLD_LOCK_THIS_BLOCK;
    typedef const char* (*funcType)(NSModule module);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSNameOfModule", &p);
	return(p(module));
} 

const char* 
NSLibraryNameForModule(
NSModule module)
{
	if ( gUseDyld3 )
		return dyld3::NSLibraryNameForModule(module);

	DYLD_LOCK_THIS_BLOCK;
    typedef const char*  (*funcType)(NSModule module);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLibraryNameForModule", &p);
	return(p(module));
}

bool
NSIsSymbolNameDefined(
const char* symbolName)
{
	if ( gUseDyld3 )
		return dyld3::NSIsSymbolNameDefined(symbolName);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const char* symbolName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSIsSymbolNameDefined", &p);
	return(p(symbolName));
}

bool
NSIsSymbolNameDefinedWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	if ( gUseDyld3 )
		return dyld3::NSIsSymbolNameDefinedWithHint(symbolName, libraryNameHint);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const char* symbolName,
                             const char* libraryNameHint);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSIsSymbolNameDefinedWithHint", &p);
	return(p(symbolName, libraryNameHint));
}

bool
NSIsSymbolNameDefinedInImage(
const struct mach_header *image,
const char* symbolName)
{
	if ( gUseDyld3 )
		return dyld3::NSIsSymbolNameDefinedInImage(image, symbolName);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const struct mach_header *image,
                             const char* symbolName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSIsSymbolNameDefinedInImage", &p);
	return(p(image, symbolName));
}

NSSymbol
NSLookupAndBindSymbol(
const char* symbolName)
{
	if ( gUseDyld3 )
		return dyld3::NSLookupAndBindSymbol(symbolName);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSSymbol (*funcType)(const char* symbolName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLookupAndBindSymbol", &p);
	return(p(symbolName));
}

NSSymbol
NSLookupAndBindSymbolWithHint(
const char* symbolName,
const char* libraryNameHint)
{
	if ( gUseDyld3 )
		return dyld3::NSLookupAndBindSymbolWithHint(symbolName, libraryNameHint);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSSymbol (*funcType)(const char* symbolName,
                                 const char* libraryNameHint);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLookupAndBindSymbolWithHint", &p);
	return(p(symbolName, libraryNameHint));
}

NSSymbol
NSLookupSymbolInModule(
NSModule module,
const char* symbolName)
{
	if ( gUseDyld3 )
		return dyld3::NSLookupSymbolInModule(module, symbolName);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSSymbol (*funcType)(NSModule module, const char* symbolName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLookupSymbolInModule", &p);
	return(p(module, symbolName));
}

NSSymbol
NSLookupSymbolInImage(
const struct mach_header *image,
const char* symbolName,
uint32_t options)
{
	if ( gUseDyld3 )
		return dyld3::NSLookupSymbolInImage(image, symbolName, options);

 	DYLD_LOCK_THIS_BLOCK;
    typedef NSSymbol (*funcType)(const struct mach_header *image,
                                 const char* symbolName,
                                 uint32_t options);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLookupSymbolInImage", &p);
	return(p(image, symbolName, options));
}

const char* 
NSNameOfSymbol(
NSSymbol symbol)
{
	if ( gUseDyld3 )
		return dyld3::NSNameOfSymbol(symbol);

	DYLD_LOCK_THIS_BLOCK;
    typedef char * (*funcType)(NSSymbol symbol);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSNameOfSymbol",&p);
	return(p(symbol));
}

void *
NSAddressOfSymbol(
NSSymbol symbol)
{
	if ( gUseDyld3 )
		return dyld3::NSAddressOfSymbol(symbol);

	DYLD_LOCK_THIS_BLOCK;
    typedef void * (*funcType)(NSSymbol symbol);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSAddressOfSymbol", &p);
	return(p(symbol));
}

NSModule
NSModuleForSymbol(
NSSymbol symbol)
{
	if ( gUseDyld3 )
		return dyld3::NSModuleForSymbol(symbol);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSModule (*funcType)(NSSymbol symbol);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSModuleForSymbol", &p);
	return(p(symbol));
}

bool
NSAddLibrary(
const char* pathName)
{
	if ( gUseDyld3 )
		return dyld3::NSAddLibrary(pathName);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const char* pathName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSAddLibrary", &p);
	return(p(pathName));
}

bool
NSAddLibraryWithSearching(
const char* pathName)
{
	if ( gUseDyld3 )
		return dyld3::NSAddLibrary(pathName);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const char* pathName);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSAddLibraryWithSearching", &p);
	return(p(pathName));
}

const struct mach_header *
NSAddImage(
const char* image_name,
uint32_t options)
{
	if ( gUseDyld3 )
		return dyld3::NSAddImage(image_name, options);

	DYLD_LOCK_THIS_BLOCK;
    typedef const struct mach_header * (*funcType)(const char* image_name,
                                                   uint32_t options);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSAddImage", &p);
	return(p(image_name, options));
}
#endif // DEPRECATED_APIS_SUPPORTED

/*
 * This routine returns the current version of the named shared library the
 * executable it was built with.  The libraryName parameter is the same as the
 * -lx or -framework Foo argument passed to the static link editor when building
 * the executable (with -lx it would be "x" and with -framework Foo it would be
 * "Foo").  If this the executable was not built against the specified library
 * it returns -1.  It should be noted that if this only returns the value the
 * current version of the named shared library the executable was built with
 * and not a list of current versions that dependent libraries and bundles the
 * program is using were built with.
 */
int32_t NSVersionOfLinkTimeLibrary(const char* libraryName)
{
	if ( gUseDyld3 )
		return dyld3::NSVersionOfLinkTimeLibrary(libraryName);

	// Lazily call _NSGetMachExecuteHeader() and cache result
#if __LP64__
    static mach_header_64* mh = NULL;
#else
    static mach_header* mh = NULL;
#endif
	if ( mh == NULL )
	    mh = _NSGetMachExecuteHeader();
#if __LP64__
	const load_command* lc = (load_command*)((char*)mh + sizeof(mach_header_64));
#else
	const load_command* lc = (load_command*)((char*)mh + sizeof(mach_header));
#endif
	for(uint32_t i = 0; i < mh->ncmds; i++){
		switch ( lc->cmd ) { 
			case LC_LOAD_DYLIB:
			case LC_LOAD_WEAK_DYLIB:
			case LC_LOAD_UPWARD_DYLIB:
				const dylib_command* dl = (dylib_command *)lc;
				const char* install_name = (char*)dl + dl->dylib.name.offset;
				if ( names_match(install_name, libraryName) )
					return dl->dylib.current_version;
				break;
		}
	    lc = (load_command*)((char*)lc + lc->cmdsize);
	}
	return (-1);
}

/*
 * This routine returns the current version of the named shared library the
 * program it is running against.  The libraryName parameter is the same as
 * would be static link editor using the -lx or -framework Foo flags (with -lx
 * it would be "x" and with -framework Foo it would be "Foo").  If the program
 * is not using the specified library it returns -1.
 */
int32_t NSVersionOfRunTimeLibrary(const char* libraryName)
{
	if ( gUseDyld3 )
		return dyld3::NSVersionOfRunTimeLibrary(libraryName);

	uint32_t n = _dyld_image_count();
	for(uint32_t i = 0; i < n; i++){
	    const mach_header* mh = _dyld_get_image_header(i);
		if ( mh == NULL )
			continue;
	    if ( mh->filetype != MH_DYLIB )
			continue;
#if __LP64__
	    const load_command* lc = (load_command*)((char*)mh + sizeof(mach_header_64));
#else
	    const load_command* lc = (load_command*)((char*)mh + sizeof(mach_header));
#endif
	    for(uint32_t j = 0; j < mh->ncmds; j++){
			if ( lc->cmd == LC_ID_DYLIB ) {
				const dylib_command* dl = (dylib_command*)lc;
				const char* install_name = (char *)dl + dl->dylib.name.offset;
				if ( names_match(install_name, libraryName) )
					return dl->dylib.current_version;
			}
			lc = (load_command*)((char*)lc + lc->cmdsize);
	    }
	}
	return (-1);
}

#if TARGET_OS_WATCH
uint32_t dyld_get_program_sdk_watch_os_version()
{
    if (gUseDyld3)
        return dyld3::dyld_get_program_sdk_watch_os_version();

    __block uint32_t retval = 0;
    __block bool versionFound = false;
    dyld3::dyld_get_image_versions((mach_header*)_NSGetMachExecuteHeader(), ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if (versionFound) return;

        if (dyld_get_base_platform(platform) == PLATFORM_WATCHOS) {
            versionFound = true;
            retval = sdk_version;
        }
    });

    return retval;
}

uint32_t dyld_get_program_min_watch_os_version()
{
    if (gUseDyld3)
        return dyld3::dyld_get_program_min_watch_os_version();

    __block uint32_t retval = 0;
    __block bool versionFound = false;
    dyld3::dyld_get_image_versions((mach_header*)_NSGetMachExecuteHeader(), ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if (versionFound) return;

        if (dyld_get_base_platform(platform) == PLATFORM_WATCHOS) {
            versionFound = true;
            retval = min_version;
        }
    });

    return retval;
}
#endif

#if TARGET_OS_BRIDGE
uint32_t dyld_get_program_sdk_bridge_os_version()
{
    if (gUseDyld3)
        return dyld3::dyld_get_program_sdk_bridge_os_version();

    __block uint32_t retval = 0;
    __block bool versionFound = false;
    dyld3::dyld_get_image_versions((mach_header*)_NSGetMachExecuteHeader(), ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if (versionFound) return;

        if (dyld_get_base_platform(platform) == PLATFORM_BRIDGEOS) {
            versionFound = true;
            retval = sdk_version;
        }
    });

    return retval;
}

uint32_t dyld_get_program_min_bridge_os_version()
{
    if (gUseDyld3)
        return dyld3::dyld_get_program_min_bridge_os_version();

    __block uint32_t retval = 0;
    __block bool versionFound = false;
    dyld3::dyld_get_image_versions((mach_header*)_NSGetMachExecuteHeader(), ^(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version) {
        if (versionFound) return;

        if (dyld_get_base_platform(platform) == PLATFORM_BRIDGEOS) {
            versionFound = true;
            retval = min_version;
        }
    });

    return retval;
}
#endif

/*
 * Returns the sdk version (encode as nibble XXXX.YY.ZZ) the
 * specified binary was built against.
 *
 * First looks for LC_VERSION_MIN_* in binary and if sdk field is 
 * not zero, return that value.
 * Otherwise, looks for the libSystem.B.dylib the binary linked
 * against and uses a table to convert that to an sdk version.
 */
uint32_t dyld_get_sdk_version(const mach_header* mh)
{
    return dyld3::dyld_get_sdk_version(mh);
}

uint32_t dyld_get_program_sdk_version()
{
    return dyld3::dyld_get_sdk_version((mach_header*)_NSGetMachExecuteHeader());
}

uint32_t dyld_get_min_os_version(const struct mach_header* mh)
{
    return dyld3::dyld_get_min_os_version(mh);
}


uint32_t dyld_get_program_min_os_version()
{
    return dyld3::dyld_get_min_os_version((mach_header*)_NSGetMachExecuteHeader());
}


bool _dyld_get_image_uuid(const struct mach_header* mh, uuid_t uuid)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_image_uuid(mh, uuid);

	const load_command* startCmds = NULL;
	if ( mh->magic == MH_MAGIC_64 )
		startCmds = (load_command*)((char *)mh + sizeof(mach_header_64));
	else if ( mh->magic == MH_MAGIC )
		startCmds = (load_command*)((char *)mh + sizeof(mach_header));
	else
		return false;  // not a mach-o file, or wrong endianness

	const load_command* const cmdsEnd = (load_command*)((char*)startCmds + mh->sizeofcmds);
	const load_command* cmd = startCmds;
	for(uint32_t i = 0; i < mh->ncmds; ++i) {
	    const load_command* nextCmd = (load_command*)((char *)cmd + cmd->cmdsize);
		if ( (cmd->cmdsize < 8) || (nextCmd > cmdsEnd) || (nextCmd < startCmds)) {
			return false;
		}
		if ( cmd->cmd == LC_UUID ) {
			const uuid_command* uuidCmd = (uuid_command*)cmd;
			memcpy(uuid, uuidCmd->uuid, 16);
			return true;
		}
		cmd = nextCmd;
	}
	bzero(uuid, 16);
	return false;
}

dyld_platform_t dyld_get_active_platform(void) {
    if (gUseDyld3)
        return dyld3::dyld_get_active_platform();

    return (dyld_platform_t)_dyld_get_all_image_infos()->platform;
}

dyld_platform_t dyld_get_base_platform(dyld_platform_t platform) {
    return dyld3::dyld_get_base_platform(platform);
}

bool dyld_is_simulator_platform(dyld_platform_t platform) {
    return dyld3::dyld_is_simulator_platform(platform);
}

bool dyld_sdk_at_least(const struct mach_header* mh, dyld_build_version_t version) {
    return dyld3::dyld_sdk_at_least(mh, version);
}

bool dyld_minos_at_least(const struct mach_header* mh, dyld_build_version_t version) {
    return dyld3::dyld_minos_at_least(mh, version);
}

bool dyld_program_sdk_at_least(dyld_build_version_t version) {
    return dyld3::dyld_program_sdk_at_least(version);
}

bool dyld_program_minos_at_least(dyld_build_version_t version) {
    return dyld3::dyld_program_minos_at_least(version);
}

// Function that walks through the load commands and calls the internal block for every version found
// Intended as a fallback for very complex (and rare) version checks, or for tools that need to
// print our everything for diagnostic reasons
void dyld_get_image_versions(const struct mach_header* mh, void (^callback)(dyld_platform_t platform, uint32_t sdk_version, uint32_t min_version)) {
    dyld3::dyld_get_image_versions(mh, callback);
}



#if DEPRECATED_APIS_SUPPORTED
/*
 * NSCreateObjectFileImageFromFile() creates an NSObjectFileImage for the
 * specified file name if the file is a correct Mach-O file that can be loaded
 * with NSloadModule().  For return codes of NSObjectFileImageFailure and
 * NSObjectFileImageFormat an error message is printed to stderr.  All
 * other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	if ( gUseDyld3 )
		return dyld3::NSCreateObjectFileImageFromFile(pathName, objectFileImage);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSObjectFileImageReturnCode (*funcType)(const char*, NSObjectFileImage*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSCreateObjectFileImageFromFile", &p);
	return p(pathName, objectFileImage);
}


/*
 * NSCreateObjectFileImageFromMemory() creates an NSObjectFileImage for the
 * object file mapped into memory at address of size length if the object file
 * is a correct Mach-O file that can be loaded with NSloadModule().  For return
 * codes of NSObjectFileImageFailure and NSObjectFileImageFormat an error
 * message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateObjectFileImageFromMemory(
const void* address,
size_t size, 
NSObjectFileImage *objectFileImage)
{
#ifndef DARLING
    // <rdar://problem/51812762> NSCreatObjectFileImageFromMemory fail opaquely if Hardened runtime is enabled
    uint32_t flags;
    if ( csops(0, CS_OPS_STATUS, &flags, sizeof(flags)) != -1 ) {
        if ( (flags & (CS_ENFORCEMENT|CS_KILL)) == (CS_ENFORCEMENT|CS_KILL) ) {
            //fprintf(stderr, "dyld: warning: NSCreatObjectFileImageFromMemory() cannot be used in harden process 0x%08X\n", flags);
            return NSObjectFileImageAccess;
        }
    }
#endif

	if ( gUseDyld3 )
		return dyld3::NSCreateObjectFileImageFromMemory(address, size, objectFileImage);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSObjectFileImageReturnCode (*funcType)(const void*, size_t, NSObjectFileImage*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSCreateObjectFileImageFromMemory", &p);
	return p(address, size, objectFileImage);
}

#if OBSOLETE_DYLD_API
/*
 * NSCreateCoreFileImageFromFile() creates an NSObjectFileImage for the 
 * specified core file name if the file is a correct Mach-O core file.
 * For return codes of NSObjectFileImageFailure and NSObjectFileImageFormat
 * an error message is printed to stderr.  All other codes cause no printing. 
 */
NSObjectFileImageReturnCode
NSCreateCoreFileImageFromFile(
const char* pathName,
NSObjectFileImage *objectFileImage)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef NSObjectFileImageReturnCode (*funcType)(const char*, NSObjectFileImage*) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSCreateCoreFileImageFromFile", &p);
	return p(pathName, objectFileImage);
}
#endif

bool
NSDestroyObjectFileImage(
NSObjectFileImage objectFileImage)
{
	if ( gUseDyld3 )
		return dyld3::NSDestroyObjectFileImage(objectFileImage);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(NSObjectFileImage);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSDestroyObjectFileImage", &p);
	return p(objectFileImage);
}


NSModule
NSLinkModule(
NSObjectFileImage objectFileImage, 
const char* moduleName,
uint32_t options)
{
	if ( gUseDyld3 )
		return dyld3::NSLinkModule(objectFileImage, moduleName, options);

	DYLD_LOCK_THIS_BLOCK;
    typedef NSModule (*funcType)(NSObjectFileImage, const char*, unsigned long);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSLinkModule", &p);
		
	return p(objectFileImage, moduleName, options);
}




/*
 * NSSymbolDefinitionCountInObjectFileImage() returns the number of symbol
 * definitions in the NSObjectFileImage.
 */
uint32_t
NSSymbolDefinitionCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	if ( gUseDyld3 )
		return dyld3::NSSymbolDefinitionCountInObjectFileImage(objectFileImage);

	DYLD_LOCK_THIS_BLOCK;
    typedef uint32_t (*funcType)(NSObjectFileImage);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSSymbolDefinitionCountInObjectFileImage", &p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolDefinitionNameInObjectFileImage() returns the name of the i'th
 * symbol definitions in the NSObjectFileImage.  If the ordinal specified is
 * outside the range [0..NSSymbolDefinitionCountInObjectFileImage], NULL will
 * be returned.
 */
const char* 
NSSymbolDefinitionNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal)
{
	if ( gUseDyld3 )
		return dyld3::NSSymbolDefinitionNameInObjectFileImage(objectFileImage, ordinal);

	DYLD_LOCK_THIS_BLOCK;
    typedef const char*  (*funcType)(NSObjectFileImage, uint32_t);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSSymbolDefinitionNameInObjectFileImage", &p);
		
	return p(objectFileImage, ordinal);
}

/*
 * NSSymbolReferenceCountInObjectFileImage() returns the number of references
 * to undefined symbols the NSObjectFileImage.
 */
uint32_t
NSSymbolReferenceCountInObjectFileImage(
NSObjectFileImage objectFileImage)
{
	if ( gUseDyld3 )
		return dyld3::NSSymbolReferenceCountInObjectFileImage(objectFileImage);

	DYLD_LOCK_THIS_BLOCK;
    typedef uint32_t (*funcType)(NSObjectFileImage);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSSymbolReferenceCountInObjectFileImage", &p);
		
	return p(objectFileImage);
}

/*
 * NSSymbolReferenceNameInObjectFileImage() returns the name of the i'th
 * undefined symbol in the NSObjectFileImage. If the ordinal specified is
 * outside the range [0..NSSymbolReferenceCountInObjectFileImage], NULL will be
 * returned.
 */
const char* 
NSSymbolReferenceNameInObjectFileImage(
NSObjectFileImage objectFileImage,
uint32_t ordinal,
bool *tentative_definition) /* can be NULL */
{
	if ( gUseDyld3 )
		return dyld3::NSSymbolReferenceNameInObjectFileImage(objectFileImage, ordinal, tentative_definition);

	DYLD_LOCK_THIS_BLOCK;
    typedef const char*  (*funcType)(NSObjectFileImage, uint32_t, bool*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSSymbolReferenceNameInObjectFileImage", &p);
		
	return p(objectFileImage, ordinal, tentative_definition);
}

/*
 * NSIsSymbolDefinedInObjectFileImage() returns TRUE if the specified symbol
 * name has a definition in the NSObjectFileImage and FALSE otherwise.
 */
bool
NSIsSymbolDefinedInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* symbolName)
{
	if ( gUseDyld3 )
		return dyld3::NSIsSymbolDefinedInObjectFileImage(objectFileImage, symbolName);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(NSObjectFileImage, const char*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSIsSymbolDefinedInObjectFileImage", &p);
		
	return p(objectFileImage, symbolName);
}

/*
 * NSGetSectionDataInObjectFileImage() returns a pointer to the section contents
 * in the NSObjectFileImage for the specified segmentName and sectionName if
 * it exists and it is not a zerofill section.  If not it returns NULL.  If
 * the parameter size is not NULL the size of the section is also returned
 * indirectly through that pointer.
 */
void *
NSGetSectionDataInObjectFileImage(
NSObjectFileImage objectFileImage,
const char* segmentName,
const char* sectionName,
unsigned long *size) /* can be NULL */
{
	if ( gUseDyld3 )
		return dyld3::NSGetSectionDataInObjectFileImage(objectFileImage, segmentName, sectionName, size);

	DYLD_LOCK_THIS_BLOCK;
    typedef void* (*funcType)(NSObjectFileImage, const char*, const char*, unsigned long*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_NSGetSectionDataInObjectFileImage", &p);
		
	return p(objectFileImage, segmentName, sectionName, size);
}


void
NSLinkEditError(
NSLinkEditErrors *c,
int *errorNumber, 
const char* *fileName,
const char* *errorString)
{
	if ( gUseDyld3 )
		return dyld3::NSLinkEditError(c, errorNumber, fileName, errorString);

	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(NSLinkEditErrors *c,
                             int *errorNumber,
                             const char* *fileName,
                             const char* *errorString);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_link_edit_error", &p);
	if(p != NULL)
	    p(c, errorNumber, fileName, errorString);
}

bool
NSUnLinkModule(
NSModule module, 
uint32_t options)
{
	if ( gUseDyld3 )
		return dyld3::NSUnLinkModule(module, options);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(NSModule module, uint32_t options);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_unlink_module", &p);

	return p(module, options);
}

#if OBSOLETE_DYLD_API
NSModule
NSReplaceModule(
NSModule moduleToReplace,
NSObjectFileImage newObjectFileImage, 
uint32_t options)
{
	return(NULL);
}
#endif


#endif // DEPRECATED_APIS_SUPPORTED

/*
 *_NSGetExecutablePath copies the path of the executable into the buffer and
 * returns 0 if the path was successfully copied in the provided buffer. If the
 * buffer is not large enough, -1 is returned and the expected buffer size is
 * copied in *bufsize. Note that _NSGetExecutablePath will return "a path" to
 * the executable not a "real path" to the executable. That is the path may be
 * a symbolic link and not the real file. And with deep directories the total
 * bufsize needed could be more than MAXPATHLEN.
 */
int
_NSGetExecutablePath(
char *buf,
uint32_t *bufsize)
{
	if ( gUseDyld3 )
		return dyld3::_NSGetExecutablePath(buf, bufsize);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef int (*funcType)(char *buf, uint32_t *bufsize);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld__NSGetExecutablePath", &p);
	return(p(buf, bufsize));
}

#if DEPRECATED_APIS_SUPPORTED
void
_dyld_lookup_and_bind(
const char* symbol_name,
void** address,
NSModule* module)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_lookup_and_bind(symbol_name, address, module);

	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(const char*, void** , NSModule*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_lookup_and_bind", &p);
	p(symbol_name, address, module);
}

void
_dyld_lookup_and_bind_with_hint(
const char* symbol_name,
const char* library_name_hint,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(const char*, const char*, void**, NSModule*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_lookup_and_bind_with_hint", &p);
	p(symbol_name, library_name_hint, address, module);
}

#if OBSOLETE_DYLD_API
void
_dyld_lookup_and_bind_objc(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(const char* , void**, NSModule*) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_lookup_and_bind_objc", &p);
	p(symbol_name, address, module);
}
#endif

void
_dyld_lookup_and_bind_fully(
const char* symbol_name,
void** address,
NSModule* module)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(const char*, void**, NSModule*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_lookup_and_bind_fully", &p);
	p(symbol_name, address, module);
}

bool
_dyld_bind_fully_image_containing_address(
const void* address)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_bind_fully_image_containing_address", &p);
	return p(address);
}
#endif // DEPRECATED_APIS_SUPPORTED


/*
 * _dyld_register_func_for_add_image registers the specified function to be
 * called when a new image is added (a bundle or a dynamic shared library) to
 * the program.  When this function is first registered it is called for once
 * for each image that is currently part of the program.
 */
void
_dyld_register_func_for_add_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	if ( gUseDyld3 )
		return dyld3::_dyld_register_func_for_add_image(func);

	DYLD_LOCK_THIS_BLOCK;
	// Func must be a "void *" because dyld itself calls it. DriverKit
	// libdyld.dylib uses diversified C function pointers but its dyld (the
	// plain OS one) doesn't, so it must be resigned with 0 discriminator.
	typedef void (*funcType)(void *func);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_register_func_for_add_image", &p);
	p(reinterpret_cast<void *>(func));
}

/*
 * _dyld_register_func_for_remove_image registers the specified function to be
 * called when an image is removed (a bundle or a dynamic shared library) from
 * the program.
 */
void
_dyld_register_func_for_remove_image(
void (*func)(const struct mach_header *mh, intptr_t vmaddr_slide))
{
	if ( gUseDyld3 )
		return dyld3::_dyld_register_func_for_remove_image(func);

	DYLD_LOCK_THIS_BLOCK;
	// Func must be a "void *" because dyld itself calls it. DriverKit
	// libdyld.dylib uses diversified C function pointers but its dyld (the
	// plain OS one) doesn't, so it must be resigned with 0 discriminator.
	typedef void (*funcType)(void *func);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_register_func_for_remove_image", &p);
	p(reinterpret_cast<void *>(func));
}

#if OBSOLETE_DYLD_API
/*
 * _dyld_register_func_for_link_module registers the specified function to be
 * called when a module is bound into the program.  When this function is first
 * registered it is called for once for each module that is currently bound into
 * the program.
 */
void
_dyld_register_func_for_link_module(
void (*func)(NSModule module))
{
	DYLD_LOCK_THIS_BLOCK;
	// Func must be a "void *" because dyld itself calls it. DriverKit
	// libdyld.dylib uses diversified C function pointers but its dyld (the
	// plain OS one) doesn't, so it must be resigned with 0 discriminator.
	static void (*funcType)(void *func) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_register_func_for_link_module", &p);
	p(reinterpret_cast<void *>(func));
}

/*
 * _dyld_register_func_for_unlink_module registers the specified function to be
 * called when a module is unbound from the program.
 */
void
_dyld_register_func_for_unlink_module(
void (*func)(NSModule module))
{
	DYLD_LOCK_THIS_BLOCK;
	// Func must be a "void *" because dyld itself calls it. DriverKit
	// libdyld.dylib uses diversified C function pointers but its dyld (the
	// plain OS one) doesn't, so it must be resigned with 0 discriminator.
	static void (*funcType)(void *func) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_register_func_for_unlink_module", &p);
	p(reinterpret_cast<void *>(func));
}

/*
 * _dyld_register_func_for_replace_module registers the specified function to be
 * called when a module is to be replace with another module in the program.
 */
void
_dyld_register_func_for_replace_module(
void (*func)(NSModule oldmodule, NSModule newmodule))
{
	DYLD_LOCK_THIS_BLOCK;
	// Func must be a "void *" because dyld itself calls it. DriverKit
	// libdyld.dylib uses diversified C function pointers but its dyld (the
	// plain OS one) doesn't, so it must be resigned with 0 discriminator.
    typedef void (*funcType)(void *func) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_register_func_for_replace_module", &p);
	p(reinterpret_cast<void *>(func));
}


/*
 * _dyld_get_objc_module_sect_for_module is passed a module and sets a 
 * pointer to the (__OBJC,__module) section and its size for the specified
 * module.
 */
void
_dyld_get_objc_module_sect_for_module(
NSModule module,
void **objc_module,
unsigned long *size)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(NSModule module,
                             void **objc_module,
                             unsigned long *size) = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_objc_module_sect_for_module", &p);
	p(module, objc_module, size);
}

#endif

#if DEPRECATED_APIS_SUPPORTED
bool
_dyld_present(void)
{
	// this function exists for compatiblity only
	return true;
}
#endif

uint32_t
_dyld_image_count(void)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_image_count();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef uint32_t (*funcType)(void);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_image_count", &p);
	return(p());
}

const struct mach_header *
_dyld_get_image_header(uint32_t image_index)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_image_header(image_index);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef struct mach_header * (*funcType)(uint32_t image_index);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_image_header", &p);
	return(p(image_index));
}

intptr_t
_dyld_get_image_vmaddr_slide(uint32_t image_index)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_image_vmaddr_slide(image_index);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef unsigned long (*funcType)(uint32_t image_index);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_image_vmaddr_slide", &p);
	return(p(image_index));
}

const char* 
_dyld_get_image_name(uint32_t image_index)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_image_name(image_index);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef const char*  (*funcType)(uint32_t image_index);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_image_name", &p);
	return(p(image_index));
}

// SPI in Mac OS X 10.6
intptr_t _dyld_get_image_slide(const struct mach_header* mh)
{
	// always use dyld3 version because it does better error handling
	return dyld3::_dyld_get_image_slide(mh);
}

const struct mach_header *
_dyld_get_prog_image_header()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_get_prog_image_header();

    DYLD_LOCK_THIS_BLOCK;
    typedef const struct mach_header * (*funcType)(void);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_get_prog_image_header", &p);
    return p();
}

//
// Apple's dyld_priv.h:222 -- `const struct mach_header* _dyld_get_dlopen_image_header(void* handle)`,
// "Exists in Mac OS X 13.0 and later", symbol __dyld_get_dlopen_image_header.
//
// WHY IT IS NEEDED.  DVTFoundation's chained-fixups import table binds it STRONG and EAGER on the
// libSystem ordinal (measured from the image's own bytes: chained import 499 of 2261, lib_ordinal 3 =
// /usr/lib/libSystem.B.dylib, weak_import 0), so the loader refuses the whole image until it
// resolves.  The SDK's own .../MacOSX.sdk/usr/lib/system/libdyld.tbd lists the name in libdyld's
// export set, and this tree's /usr/lib/libSystem.B.dylib re-exports libdyld, so this file -- libdyld
// -- is the image that must answer it.
//
// THE CALLER, measured rather than assumed.  Its ONE call site is 0x229b22, in
// +[DVTAbstractMethodChecker machHeaderAddressForLoadedBundle:]: it opens the bundle's executable
// with `dlopen(path, 0x10)` -- 0x10 is RTLD_NOLOAD, a probe of an image the process already has --
// keeps the handle in %rbx, returns early if the handle is NULL, and otherwise passes exactly that
// dlopen handle here, saves the answer, dlclose()s the handle, and RETURNS THE ANSWER to its caller
// (the whole rest of the method is `movq %rbx,%rdi; callq dlclose; movq %r14,%rax; retq`).  So the
// two shapes that must never be produced are a NULL answer for a handle this process really got from
// dlopen (the caller's nil-test precedes the call, so the answer is used, not tested) and a
// fabricated pointer (it is handed to -[DVTAbstractMethodChecker
// abstractMethodRecordsInLibraryWithMachHeader:] and then to getsectiondata()).  An explicit failure
// is the contract: NULL for a handle that is not one this process's loader issued.
//
// THE CONTRACT, Apple's and unchanged: the mach_header of the image the handle names; RTLD_SELF
// answers with the image containing the caller; RTLD_MAIN_ONLY with the main executable; anything
// else that is not a live handle answers NULL, which is what Apple's `loaderFromHandle` +
// `validLoader` pair produces ("if an invalid `handle` passed in, return NULL").
//
// TWO HANDLE ENCODINGS, because this tree's dyld has Apple's two launch modes: libdyld's dlopen
// forwards to `dyld3::dlopen_internal` when the process launched through a closure (gUseDyld3), and
// otherwise to the loader, which hands out its own `ImageLoader*` (its low bits carry RTLD_FIRST).
// The two are decoded by their own implementation; neither is re-derived here, and the same
// gUseDyld3 branch is what every sibling API in this file uses.  Measured: THIS guest takes the
// legacy path -- dyld2.cpp's default closure mode on x86_64 macOS is `PreBuiltOnly`, a closure can
// only come from a dyld shared cache (`sSharedCacheLoadInfo.loadAddress->findClosure`), and the
// deployed prefix holds no dyld_shared_cache* file at all -- so the frame the caller reaches is the
// loader's implementation, and the dyld3 branch is the one Apple's own architecture requires rather
// than one this workload exercises.
// Declared here, not in a header: this tree's <mach-o/dyld_priv.h> is the dyld-733 generation and
// predates the macOS 13.0 declaration, and a definition with C++ linkage would export a mangled
// symbol the loader could never bind (the name the client's bind table asks libSystem.B for is the
// C one, __dyld_get_dlopen_image_header).
extern "C" {
const struct mach_header* _dyld_get_dlopen_image_header(void* handle);
}

const struct mach_header *
_dyld_get_dlopen_image_header(void* handle)
{
    if ( gUseDyld3 )
        return dyld3::_dyld_get_dlopen_image_header(handle);

    DYLD_LOCK_THIS_BLOCK;
    typedef const struct mach_header * (*funcType)(void* handle);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_get_dlopen_image_header", &p);
    return p(handle);
}

#if DEPRECATED_APIS_SUPPORTED
bool
_dyld_image_containing_address(const void* address)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_image_containing_address(address);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_image_containing_address", &p);
	return(p(address));
}

const struct mach_header *
_dyld_get_image_header_containing_address(
const void* address)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_image_header_containing_address(address);

	DYLD_LOCK_THIS_BLOCK;
    typedef const struct mach_header * (*funcType)(const void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_image_header_containing_address", &p);
	return p(address);
}

bool _dyld_launched_prebound(void)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(void);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_launched_prebound", &p);
	return(p());
}

bool _dyld_all_twolevel_modules_prebound(void)
{
	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(void);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_all_twolevel_modules_prebound", &p);
	return(p());
}
#endif // DEPRECATED_APIS_SUPPORTED


#include <dlfcn_private.h>
#include <stddef.h>
#include <pthread.h>
#include <stdlib.h>
#include <mach-o/dyld.h>
#include "dyldLibSystemInterface.h"


// pthread key used to access per-thread dlerror message
static pthread_key_t dlerrorPerThreadKey;
static bool dlerrorPerThreadKeyInitialized = false;

// data kept per-thread
struct dlerrorPerThreadData
{
	size_t		sizeAllocated;
	char		message[1];
};

// function called by dyld to get buffer to store dlerror message
static char* getPerThreadBufferFor_dlerror(size_t sizeRequired)
{
	// ok to create key lazily because this function is called within dyld lock, so there is no race condition
	if (!dlerrorPerThreadKeyInitialized ) {
		// create key and tell pthread package to call free() on any data associated with key if thread dies
		pthread_key_create(&dlerrorPerThreadKey, &free);
		dlerrorPerThreadKeyInitialized = true;
	}

	const size_t size = (sizeRequired < 256) ? 256 : sizeRequired;
	dlerrorPerThreadData* data = (dlerrorPerThreadData*)pthread_getspecific(dlerrorPerThreadKey);
	if ( data == NULL ) {
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const size_t mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	else if ( data->sizeAllocated < sizeRequired ) {
		free(data);
		//int mallocSize = offsetof(dlerrorPerThreadData, message[size]);
		const size_t mallocSize = sizeof(dlerrorPerThreadData)+size;
		data = (dlerrorPerThreadData*)malloc(mallocSize);
		data->sizeAllocated = size;
		pthread_setspecific(dlerrorPerThreadKey, data);
	}
	return data->message;
}

// <rdar://problem/10595338> dlerror buffer leak
// Only allocate buffer if an actual error message needs to be set
static bool hasPerThreadBufferFor_dlerror()
{
	if (!dlerrorPerThreadKeyInitialized ) 
		return false;
		
	return (pthread_getspecific(dlerrorPerThreadKey) != NULL);
}

#if TARGET_OS_DRIVERKIT
static bool isLaunchdOwned()
{
    return false;
}
#else
// use non-lazy pointer to vproc_swap_integer so that lazy binding does not recurse
typedef vproc_err_t (*vswapproc)(vproc_t vp, vproc_gsk_t key,int64_t *inval, int64_t *outval);
static vswapproc swapProc = &vproc_swap_integer;

static bool isLaunchdOwned()
{
    static bool checked = false;
    static bool result = false;
    if ( !checked ) {
        checked = true;
	    int64_t val = 0;
	    (*swapProc)(NULL, VPROC_GSK_IS_MANAGED, NULL, &val);
	    result = ( val != 0 );
    }
    return result;
}
#endif

static void shared_cache_missing()
{
	// leave until dyld's that might call this are rare
}

static void shared_cache_out_of_date()
{
	// leave until dyld's that might call this are rare
}

// FIXME: This is a mess. Why can't Driverkit have its own dyld?
static int cxa_atexit_thunk(void (*func)(void *), void *arg, void *dso)
{
	// Func will have come from dyld and so be signed with 0 discriminator,
	// resign it appropriately before passing to the real __cxa_atexit.
	func = ptrauth_auth_and_resign(func, ptrauth_key_function_pointer, 0,
	    ptrauth_key_function_pointer,
	    ptrauth_function_pointer_type_discriminator(__typeof__(func)));
	return __cxa_atexit(func, arg, dso);
}

template<typename FTy> static FTy *resign_for_dyld(FTy *func) {
	return ptrauth_auth_and_resign(func, ptrauth_key_function_pointer,
	    ptrauth_function_pointer_type_discriminator(__typeof__(func)),
	    ptrauth_key_function_pointer, 0);
}


// the table passed to dyld containing thread helpers
static dyld::LibSystemHelpers sHelpers = { 13 };

static const objc_opt::objc_opt_t* gObjCOpt = nullptr;
//
// during initialization of libSystem this routine will run
// and call dyld, registering the helper functions.
//
extern "C" void tlv_initializer();
void _dyld_initializer()
{
    sHelpers.acquireGlobalDyldLock = resign_for_dyld(&dyldGlobalLockAcquire);
    sHelpers.releaseGlobalDyldLock = resign_for_dyld(&dyldGlobalLockRelease);
    sHelpers.getThreadBufferFor_dlerror = resign_for_dyld(&getPerThreadBufferFor_dlerror);
    sHelpers.malloc = resign_for_dyld(&malloc);
    sHelpers.free = resign_for_dyld(&free);
    sHelpers.cxa_atexit = resign_for_dyld(&cxa_atexit_thunk);
    sHelpers.dyld_shared_cache_missing = resign_for_dyld(&shared_cache_missing);
    sHelpers.dyld_shared_cache_out_of_date = resign_for_dyld(&shared_cache_out_of_date);
    sHelpers.acquireDyldInitializerLock = NULL;
    sHelpers.releaseDyldInitializerLock = NULL;
    sHelpers.pthread_key_create = resign_for_dyld(&pthread_key_create);
    sHelpers.pthread_setspecific = resign_for_dyld(&pthread_setspecific);
    sHelpers.malloc_size = resign_for_dyld(&malloc_size);
    sHelpers.pthread_getspecific = resign_for_dyld(&pthread_getspecific);
    sHelpers.cxa_finalize = resign_for_dyld(&__cxa_finalize);
    sHelpers.startGlueToCallExit = address_of_start;
    sHelpers.hasPerThreadBufferFor_dlerror = resign_for_dyld(&hasPerThreadBufferFor_dlerror);
    sHelpers.isLaunchdOwned = resign_for_dyld(&isLaunchdOwned);
    sHelpers.vm_alloc = resign_for_dyld(&vm_allocate);
    sHelpers.mmap = resign_for_dyld(&mmap);
    sHelpers.cxa_finalize_ranges = resign_for_dyld(&__cxa_finalize_ranges);

    typedef void (*funcType)(dyld::LibSystemHelpers*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    // Get the optimized objc pointer now that the cache is loaded
    const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
    if ( allInfo != nullptr  ) {
        const DyldSharedCache* cache = (const DyldSharedCache*)(allInfo->sharedCacheBaseAddress);
        if ( cache != nullptr )
            gObjCOpt = cache->objcOpt();
    }

	if ( gUseDyld3 ) {
		dyld3::gAllImages.applyInitialImages();
#if TARGET_OS_IOS && !TARGET_OS_SIMULATOR
        // For binaries built before 13.0, set the lookup function if they need it
        if (dyld_get_program_sdk_version() < DYLD_PACKED_VERSION(13,0,0))
            setLookupFunc((void*)&dyld3::compatFuncLookup);
#endif
	}
	else {
		dyld_func_lookup_and_resign("__dyld_register_thread_helpers", &p);
		if(p != NULL)
			p(&sHelpers);
	}

	tlv_initializer();
}

int dladdr(const void* addr, Dl_info* info)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLADDR, (uint64_t)addr, 0, 0);
    int result = 0;
    if ( gUseDyld3 ) {
        result = dyld3::dladdr(addr, info);
    } else {
        DYLD_LOCK_THIS_BLOCK;
        typedef int (*funcType)(const void* , Dl_info*);
        static funcType __ptrauth_dyld_function_ptr p = NULL;

        if(p == NULL)
            dyld_func_lookup_and_resign("__dyld_dladdr", &p);
        result = p(addr, info);
    }
    timer.setData4(result);
    timer.setData5(info != NULL ? info->dli_fbase : 0);
    timer.setData6(info != NULL ? info->dli_saddr : 0);
    return result;
}

#if !TARGET_OS_DRIVERKIT
char* dlerror()
{
	if ( gUseDyld3 )
		return dyld3::dlerror();

	DYLD_LOCK_THIS_BLOCK;
    typedef char* (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_dlerror", &p);
	return(p());
}

int dlclose(void* handle)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLCLOSE, (uint64_t)handle, 0, 0);
    int result = 0;
    if ( gUseDyld3 ) {
        timer.setData4(result);
		return dyld3::dlclose(handle);
    }

	DYLD_LOCK_THIS_BLOCK;
    typedef int (*funcType)(void* handle);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_dlclose", &p);
    result = p(handle);
    timer.setData4(result);
	return result;
}

static void* dlopen_internal(const char* path, int mode, void* callerAddress)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLOPEN, path, mode, 0);
    void* result = nullptr;
    if ( gUseDyld3 ) {
        result = dyld3::dlopen_internal(path, mode, callerAddress);
        timer.setData4(result);
        return result;
    }

    // dlopen is special. locking is done inside dyld to allow initializer to run without lock
    DYLD_NO_LOCK_THIS_BLOCK;

    typedef void* (*funcType)(const char* path, int, void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_dlopen_internal", &p);
    result = p(path, mode, callerAddress);
    // use asm block to prevent tail call optimization
    // this is needed because dlopen uses __builtin_return_address() and depends on this glue being in the frame chain
    // <rdar://problem/5313172 dlopen() looks too far up stack, can cause crash>
    __asm__ volatile("");
    timer.setData4(result);

	return result;
}

void* dlopen(const char* path, int mode)
{
    void* result = dlopen_internal(path, mode, __builtin_return_address(0));
    if ( result )
        return result;


    return nullptr;
}

void* dlopen_from(const char* path, int mode, void* addressInCaller)
{
#if __has_feature(ptrauth_calls)
    addressInCaller = __builtin_ptrauth_strip(addressInCaller, ptrauth_key_asia);
#endif
    return dlopen_internal(path, mode, addressInCaller);
}

#if !__i386__
void* dlopen_audited(const char* path, int mode)
{
	return dlopen(path, mode);
}
#endif // !__i386__

bool dlopen_preflight(const char* path)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLOPEN_PREFLIGHT, path, 0, 0);
    bool result = false;

    if ( gUseDyld3 ) {
        result = dyld3::dlopen_preflight_internal(path);
        timer.setData4(result);
        return result;
    }

    DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const char* path, void* callerAddress);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_dlopen_preflight_internal", &p);
    result = p(path, __builtin_return_address(0));
    timer.setData4(result);
    return result;
}

void* dlsym(void* handle, const char* symbol)
{
    dyld3::ScopedTimer timer(DBG_DYLD_TIMING_DLSYM, handle, symbol, 0);
    void* result = nullptr;

    if ( gUseDyld3 ) {
        result = dyld3::dlsym_internal(handle, symbol, __builtin_return_address(0));
        timer.setData4(result);
        return result;
    }

    DYLD_LOCK_THIS_BLOCK;
    typedef void* (*funcType)(void* handle, const char* symbol, void *callerAddress);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_dlsym_internal", &p);
    result = p(handle, symbol, __builtin_return_address(0));
    timer.setData4(result);
    return result;
}
#endif // !TARGET_OS_DRIVERKIT


const struct dyld_all_image_infos* _dyld_get_all_image_infos()
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_all_image_infos();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef struct dyld_all_image_infos* (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_all_image_infos", &p);
	return p();
}

#if SUPPORT_ZERO_COST_EXCEPTIONS
bool _dyld_find_unwind_sections(void* addr, dyld_unwind_sections* info)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_find_unwind_sections(addr, info);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef void* (*funcType)(void*, dyld_unwind_sections*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_find_unwind_sections", &p);
	return p(addr, info);
}
#endif


#if __i386__ || __x86_64__ || __arm__ || __arm64__
__attribute__((visibility("hidden"))) 
void* _dyld_fast_stub_entry(void* loadercache, long lazyinfo)
{
	DYLD_NO_LOCK_THIS_BLOCK;
    typedef void* (*funcType)(void*, long);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_fast_stub_entry", &p);
	return p(loadercache, lazyinfo);
}
#endif


const char* dyld_image_path_containing_address(const void* addr)
{
	if ( gUseDyld3 )
		return dyld3::dyld_image_path_containing_address(addr);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef const char* (*funcType)(const void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_image_path_containing_address", &p);
	return p(addr);
}

const struct mach_header* dyld_image_header_containing_address(const void* addr)
{
	if ( gUseDyld3 )
		return dyld3::dyld_image_header_containing_address(addr);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef const mach_header* (*funcType)(const void*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_image_header_containing_address", &p);
	return p(addr);
}


bool dyld_shared_cache_some_image_overridden()
{
	if ( gUseDyld3 )
		return dyld3::dyld_shared_cache_some_image_overridden();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef bool (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_shared_cache_some_image_overridden", &p);
	return p();
}

bool _dyld_get_shared_cache_uuid(uuid_t uuid)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_shared_cache_uuid(uuid);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(uuid_t);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_shared_cache_uuid", &p);
	return p(uuid);
}

const void* _dyld_get_shared_cache_range(size_t* length)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_get_shared_cache_range(length);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef const void* (*funcType)(size_t*);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_get_shared_cache_range", &p);
	return p(length);
}

bool _dyld_shared_cache_optimized()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_shared_cache_optimized();

    const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
    if ( allInfo != nullptr  ) {
        const dyld_cache_header* cacheHeader = (dyld_cache_header*)(allInfo->sharedCacheBaseAddress);
        if ( cacheHeader != nullptr )
            return (cacheHeader->cacheType == kDyldSharedCacheTypeProduction);
    }
    return false;
}

bool _dyld_shared_cache_is_locally_built()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_shared_cache_is_locally_built();

    const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
    if ( allInfo != nullptr  ) {
        const dyld_cache_header* cacheHeader = (dyld_cache_header*)(allInfo->sharedCacheBaseAddress);
        if ( cacheHeader != nullptr )
            return (cacheHeader->locallyBuiltCache == 1);
    }
    return false;
}

const char* _dyld_shared_cache_real_path(const char* path)
{
    const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
    if ( allInfo != nullptr  ) {
        const DyldSharedCache* cache = (const DyldSharedCache*)(allInfo->sharedCacheBaseAddress);
        if ( cache != nullptr )
            return cache->getCanonicalPath(path);
    }
    return nullptr;
}

bool _dyld_shared_cache_contains_path(const char* path)
{
    return _dyld_shared_cache_real_path(path) != nullptr;
}


uint32_t _dyld_launch_mode()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_launch_mode();

    // in dyld2 mode all flag bits are zero
    return 0;
}

void _dyld_images_for_addresses(unsigned count, const void* addresses[], struct dyld_image_uuid_offset infos[])
{
    if ( gUseDyld3 )
        return dyld3::_dyld_images_for_addresses(count, addresses, infos);

    DYLD_NO_LOCK_THIS_BLOCK;
    typedef const void (*funcType)(unsigned, const void*[], struct dyld_image_uuid_offset[]);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_images_for_addresses", &p);
    return p(count, addresses, infos);
}

void _dyld_register_for_image_loads(void (*func)(const mach_header* mh, const char* path, bool unloadable))
{
    if ( gUseDyld3 )
        return dyld3::_dyld_register_for_image_loads(func);

    DYLD_NO_LOCK_THIS_BLOCK;
    typedef const void (*funcType)(void (*)(const mach_header* mh, const char* path, bool unloadable));
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_register_for_image_loads", &p);
    return p(func);
}

void _dyld_register_for_bulk_image_loads(void (*func)(unsigned imageCount, const struct mach_header* mhs[], const char* paths[]))
{
    if ( gUseDyld3 )
        return dyld3::_dyld_register_for_bulk_image_loads(func);

    DYLD_NO_LOCK_THIS_BLOCK;
    typedef const void (*funcType)(void (*)(unsigned imageCount, const mach_header* mhs[], const char* paths[]));
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_register_for_bulk_image_loads", &p);
    return p(func);
}

bool dyld_need_closure(const char* execPath, const char* dataContainerRootDir)
{
    return dyld3::dyld_need_closure(execPath, dataContainerRootDir);
}

bool dyld_process_is_restricted()
{
	if ( gUseDyld3 )
		return dyld3::dyld_process_is_restricted();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef bool (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;
	
	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_process_is_restricted", &p);
	return p();
}

const char* dyld_shared_cache_file_path()
{
	if ( gUseDyld3 )
		return dyld3::dyld_shared_cache_file_path();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef const char* (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;
	
	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_shared_cache_file_path", &p);
	return p();
}

bool dyld_has_inserted_or_interposing_libraries()
{
	if ( gUseDyld3 )
		return dyld3::dyld_has_inserted_or_interposing_libraries();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef bool (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if (p == NULL)
	    dyld_func_lookup_and_resign("__dyld_has_inserted_or_interposing_libraries", &p);
	return p();
}

bool _dyld_has_fix_for_radar(const char *rdar) {
    // There is no point in shimming this to dyld3, actual functionality can exist purely in libSystem for
    // both dyld2 and dyld3.
    return false;
}


void dyld_dynamic_interpose(const struct mach_header* mh, const struct dyld_interpose_tuple array[], size_t count)
{
	if ( gUseDyld3 )
		return dyld3::dyld_dynamic_interpose(mh, array, count);

	DYLD_LOCK_THIS_BLOCK;
    typedef void (*funcType)(const struct mach_header* mh, const struct dyld_interpose_tuple array[], size_t count);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if (p == NULL)
	    dyld_func_lookup_and_resign("__dyld_dynamic_interpose", &p);
	p(mh, array, count);
}

// SPI called __fork
void _dyld_atfork_prepare()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_atfork_prepare();
}

// SPI called __fork
void _dyld_atfork_parent()
{
    if ( gUseDyld3 )
        return dyld3::_dyld_atfork_parent();
}

// SPI called __fork
void _dyld_fork_child()
{
	if ( gUseDyld3 )
		return dyld3::_dyld_fork_child();

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef void (*funcType)();
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_fork_child", &p);
	return p();
}



static void* mapStartOfCache(const char* path, size_t length)
{
	struct stat statbuf;
	if ( dyld3::stat(path, &statbuf) == -1 )
		return NULL;

	if ( (size_t)statbuf.st_size < length )
		return NULL;

	int cache_fd = dyld3::open(path, O_RDONLY, 0);
	if ( cache_fd < 0 )
		return NULL;

	void* result = ::mmap(NULL, length, PROT_READ, MAP_PRIVATE, cache_fd, 0);
	close(cache_fd);

	if ( result == MAP_FAILED )
		return NULL;

	return result;
}


static const dyld_cache_header* findCacheInDirAndMap(const uuid_t cacheUuid, const char* dirPath)
{
	DIR* dirp = ::opendir(dirPath);
	if ( dirp != NULL) {
		dirent entry;
		dirent* entp = NULL;
		char cachePath[PATH_MAX];
		while ( ::readdir_r(dirp, &entry, &entp) == 0 ) {
			if ( entp == NULL )
				break;
			if ( entp->d_type != DT_REG ) 
				continue;
			if ( strlcpy(cachePath, dirPath, PATH_MAX) >= PATH_MAX )
				continue;
			if ( strlcat(cachePath, "/", PATH_MAX) >= PATH_MAX )
				continue;
			if ( strlcat(cachePath, entp->d_name, PATH_MAX) >= PATH_MAX )
				continue;
			if ( const dyld_cache_header* cacheHeader = (dyld_cache_header*)mapStartOfCache(cachePath, 0x00100000) ) {
				if ( (::memcmp(cacheHeader, "dyld_", 5) != 0) || (::memcmp(cacheHeader->uuid, cacheUuid, 16) != 0) ) {
					// wrong uuid, unmap and keep looking
					::munmap((void*)cacheHeader, 0x00100000);
				}
				else {
					// found cache
					closedir(dirp);
					return cacheHeader;
				}
			}
		}
		closedir(dirp);
	}
	return NULL;
}

int dyld_shared_cache_find_iterate_text(const uuid_t cacheUuid, const char* extraSearchDirs[], void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
	if ( gUseDyld3 )
		return dyld3::dyld_shared_cache_find_iterate_text(cacheUuid, extraSearchDirs, callback);

	const dyld_cache_header* cacheHeader = NULL;
	bool needToUnmap = true;

	// get info from dyld about this process, to see if requested cache is already mapped into this process
	const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
	if ( (allInfo != NULL) && (allInfo->sharedCacheBaseAddress != 0) && (memcmp(allInfo->sharedCacheUUID, cacheUuid, 16) == 0) ) {
		// requested cache is already mapped, just re-use it
		cacheHeader = (dyld_cache_header*)(allInfo->sharedCacheBaseAddress);
		needToUnmap = false;
	}
	else {
		// look first is default location for cache files
    #if TARGET_OS_IPHONE
		cacheHeader = findCacheInDirAndMap(cacheUuid, IPHONE_DYLD_SHARED_CACHE_DIR);
    #else
		cacheHeader = findCacheInDirAndMap(cacheUuid, MACOSX_MRM_DYLD_SHARED_CACHE_DIR);
    #endif
		// if not there, look in extra search locations
		if ( cacheHeader == NULL ) {
			for (const char** p = extraSearchDirs; *p != NULL; ++p) {
				cacheHeader = findCacheInDirAndMap(cacheUuid, *p);
				if ( cacheHeader != NULL )
					break;
			}
		}
	}

	if ( cacheHeader == NULL )
		return -1;
	
	if ( cacheHeader->mappingOffset <= __offsetof(dyld_cache_header, imagesTextOffset) ) {
		// old cache without imagesText array
		if ( needToUnmap )
			::munmap((void*)cacheHeader, 0x00100000);
		return -1;
	}

	// walk imageText table and call callback for each entry
	const dyld_cache_mapping_info* mappings = (dyld_cache_mapping_info*)((char*)cacheHeader + cacheHeader->mappingOffset);
	const uint64_t cacheUnslidBaseAddress = mappings[0].address;
	const dyld_cache_image_text_info* imagesText = (dyld_cache_image_text_info*)((char*)cacheHeader + cacheHeader->imagesTextOffset);
	const dyld_cache_image_text_info* imagesTextEnd = &imagesText[cacheHeader->imagesTextCount];
	for (const dyld_cache_image_text_info* p=imagesText; p < imagesTextEnd; ++p) {
		dyld_shared_cache_dylib_text_info dylibTextInfo;
		dylibTextInfo.version			= 2;
		dylibTextInfo.loadAddressUnslid = p->loadAddress;
		dylibTextInfo.textSegmentSize	= p->textSegmentSize;
		dylibTextInfo.path				= (char*)cacheHeader + p->pathOffset;
		::memcpy(dylibTextInfo.dylibUuid, p->uuid, 16);
		dylibTextInfo.textSegmentOffset = p->loadAddress - cacheUnslidBaseAddress;
		callback(&dylibTextInfo);
	}

	if ( needToUnmap )
		::munmap((void*)cacheHeader, 0x00100000);

	return 0;
}

int dyld_shared_cache_iterate_text(const uuid_t cacheUuid, void (^callback)(const dyld_shared_cache_dylib_text_info* info))
{
	if ( gUseDyld3 )
		return dyld3::dyld_shared_cache_iterate_text(cacheUuid, callback);

	const char* extraSearchDirs[] = { NULL };
	return dyld_shared_cache_find_iterate_text(cacheUuid, extraSearchDirs, callback);
}


//
// The dyld_introspection SPI family
// =================================
//
// This is APPLE'S dyld_introspection SPI, ported -- not re-derived -- from the dyld-1125.5 tag,
// which is the dyld macOS 14 shipped: the version this guest reports in
// /System/Library/CoreServices/SystemVersion.plist (14.0, 23A344) and the generation the
// CoreSymbolicationDT that binds these symbols was built against.  CoreSymbolicationDT binds all
// of them STRONG and EAGER through a chained-fixups import on the libSystem ordinal, so the loader
// refuses the image until every one of them resolves, whether or not it is ever called.
//
// The entry points are a port of libdyld_introspection/dyld_introspection.cpp; the semantics each
// one carries are a port of the objects Apple's own code delegates to:
//
//   common/ProcessAtlas.cpp          Atlas::Process / ProcessSnapshot / Image / SharedCache
//   common/DyldSharedCache.cpp       DyldSharedCache::isSubCachePath()
//   framework/LegacyAPIShims.mm      the null guards of the non-Atlas entry points
//
// ON THE HANDLE TYPES.  Apple hands the block an object (dyld4::Atlas::Image,
// dyld4::Atlas::SharedCache; the legacy shims hand out _DYImage / _DYSharedCache), never a raw file
// byte pointer, because the callees reach back into it: _dyld_image_content_for_segment needs the
// shared-cache slide and the buffer backing the image, and _dyld_image_get_file_path must answer
// NULL for an image that lives only in the shared cache and the file path otherwise.  This tree has
// no Atlas layer, so the same two objects are ported here over what this tree does have:
//
//   * dyld_shared_cache_t is a mapped shared cache: the mapped DyldSharedCache (whose first
//     member is the dyld_cache_header, so this is the very object dyld_shared_cache_find_iterate_text()
//     maps and hands around, at the same address as before), plus where its file is mapped, the
//     address it answers at, its unslid base and whether the mapping is private.  This mirrors
//     Atlas::SharedCache::createForFileRecord(), which maps the cache file for the same reason.
//   * dyld_image_t is a mapped Mach-O (Atlas Image::ml()) plus the two things a bare Mach-O pointer
//     cannot answer: the shared-cache slide (Atlas Image::_sharedCacheSlide) and the backing file
//     (Atlas Image::_file).  For an image from a cache the path is NULL, exactly as
//     Atlas::Image::filename() returns, and its content is the cache mapping; for an image loaded
//     in a process the path is the one the process's dyld_all_image_infos carries, and its content
//     is read from that file -- Apple's contentForSegment() materializes the on-disk bytes through
//     the file mapper, so this is that same content, not the process's patched memory.
//   * dyld_process_t / dyld_process_snapshot_t are Apple's Process / ProcessSnapshot.  This tree's
//     generation already ships a reader of exactly the data Apple's newer Process::getSnapshot()
//     reads -- _dyld_process_info_create() in src/dyld_process_info.cpp reads the target's
//     task_info(TASK_DYLD_INFO) / dyld_all_image_infos and yields the loaded images with their
//     uuid, path and segments -- so the new SPI is ported onto that rather than re-derived.  Apple's
//     failure values are kept: a target whose all_image_infos cannot be read gives a snapshot that
//     is invalid (NULL out of create), a target without a dyld shared cache gives
//     dyld_process_snapshot_get_shared_cache() NULL, and an image whose backing file is gone or
//     whose uuid does not match answers false / NULL.
//
// ON THIS GUEST THE TWO DATA SOURCES ARE MEASURED ABSENT: there is no dyld shared cache anywhere in
// the deployed prefix (no *.dsc, no dyld_shared_cache*, no com.apple.dyld; every directory below
// fails realpath() and /System/Library/Caches is empty), and the prefix maps no cache into any
// process, so the enumeration calls its block zero times and every cache handle is NULL.  A
// fabricated cache, uuid or object would be a lie; zero calls is what Apple's own code yields for a
// system with no cache installed, which is what this guest is.
//
typedef struct dyld_process_s*          dyld_process_t;
typedef struct dyld_process_snapshot_s* dyld_process_snapshot_t;
typedef struct dyld_shared_cache_s*     dyld_shared_cache_t;
typedef struct dyld_image_s*            dyld_image_t;

struct dyld_shared_cache_s {
	const DyldSharedCache* cache;          // the mapped cache: its first member is the header
	const uint8_t*                contentBase;    // where the cache file is mapped
	uint64_t                      rebasedAddress; // Atlas SharedCache::rebasedAddress()
	uint64_t                      unslidBase;     // Atlas SharedCache::_slide is rebasedAddress-unslidBase
	bool                          mappedPrivate;  // Atlas SharedCache::isPrivateMapped()
	size_t                        sizeMapped;     // non-zero when this handle owns the mapping
};

struct dyld_image_s {
	const dyld3::MachOFile* ml;                 // Atlas Image::ml(), once resolvable
	uint64_t                rebasedAddress;     // Atlas Image::rebasedAddress()
	bool                    inSharedCache;      // Atlas Image::_sharedCacheSlide.has_value()
	uint64_t                sharedCacheSlide;   // Atlas Image::_sharedCacheSlide
	const uint8_t*          contentBase;        // cache images: the cache mapping
	uint64_t                contentUnslidBase;  // cache images: the cache's unslid base
	const char*             path;               // Atlas Image::filename(); NULL for a cache image
	uuid_t                  expectedUuid;       // the uuid the process reports for this image
	bool                    hasExpectedUuid;
	bool                    mlResolved;
	void*                   mappedFile;         // owned mapping, for an image that is not in a cache
	size_t                  mappedFileSize;
	uint64_t                fileSliceOffset;
};

struct dyld_process_s {
	task_t            task;
	dyld_process_info info;      // Apple's Process::_snapshot, from this tree's all_image_infos reader
};

struct dyld_process_snapshot_s {
	dyld_process_info   info;
	dyld_shared_cache_t cache;   // Atlas ProcessSnapshot::_sharedCache; owned, NULL when there is none
	bool                cacheResolved;
};

extern "C" {
void dyld_for_each_installed_shared_cache(void (^callback)(dyld_shared_cache_t cache));
dyld_process_t dyld_process_create_for_task(task_read_t task, kern_return_t *kr);
void dyld_process_dispose(dyld_process_t process);
dyld_process_snapshot_t dyld_process_snapshot_create_for_process(dyld_process_t process, kern_return_t *kr);
void dyld_process_snapshot_dispose(dyld_process_snapshot_t snapshot);
void dyld_process_snapshot_for_each_image(dyld_process_snapshot_t snapshot, void (^block)(dyld_image_t image));
dyld_shared_cache_t dyld_process_snapshot_get_shared_cache(dyld_process_snapshot_t snapshot);
uint64_t dyld_shared_cache_get_base_address(dyld_shared_cache_t cache_atlas);
uint64_t dyld_shared_cache_get_mapped_size(dyld_shared_cache_t cache_atlas);
bool dyld_shared_cache_is_mapped_private(dyld_shared_cache_t cache_atlas);
void dyld_shared_cache_copy_uuid(dyld_shared_cache_t cache_atlas, uuid_t *uuid);
void dyld_shared_cache_for_each_image(dyld_shared_cache_t cache, void (^block)(dyld_image_t image));
bool dyld_image_content_for_segment(dyld_image_t image, const char* segmentName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize));
bool dyld_image_copy_uuid(dyld_image_t image, uuid_t* uuid);
bool dyld_image_for_each_segment_info(dyld_image_t image,
                                      void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm));
const char* dyld_image_get_installname(dyld_image_t image);
const char* dyld_image_get_file_path(dyld_image_t image);
}

// Apple's DyldSharedCache::isSubCachePath(): a leaf name carrying any suffix other than
// ".development" is a sub-cache or an auxiliary file, and the main cache is what is enumerated.
static bool isSharedCacheSubCacheLeafName(const char* leafName)
{
	const char* firstDot = strchr(leafName, '.');
	// check for files with a suffix, to know whether or not they are sub-caches
	if ( firstDot != NULL ) {
		// skip files that are not of the format "<baseName>.development", as they are sub-caches
		if ( strcmp(firstDot, ".development") != 0 )
			return true;
	}
	return false;
}

// The cache directories Apple searches, in Apple's order (common/ProcessAtlas.cpp, dyld-1125.5).
static const char* sInstalledSharedCacheDirs[] = {
	"/System/Volumes/Preboot/Cryptexes/OS/System/Library/dyld/",
	"/System/Volumes/Preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/",
	"/private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
	"/private/preboot/Cryptexes/OS/System/DriverKit/System/Library/dyld/",
	"/System/Cryptexes/OS/System/Library/Caches/com.apple.dyld/",
	"/System/Cryptexes/OS/System/Library/dyld/",
	"/System/Cryptexes/ExclaveOS/System/ExclaveKit/System/Library/dyld/",
	"/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/Library/dyld/",
	"/System/Volumes/Preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
	"/private/preboot/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
	"/private/preboot/Cryptexes/Incoming/OS/System/DriverKit/System/Library/dyld/",
	"/System/Cryptexes/Incoming/OS/System/Library/Caches/com.apple.dyld/",
	"/System/Cryptexes/Incoming/OS/System/Library/dyld/",
	"/System/Library/Caches/com.apple.dyld/",
	"/System/DriverKit/System/Library/dyld/",
	"/System/ExclaveKit/System/Library/dyld/",
	"/System/Library/dyld/"
};
#define INSTALLED_CACHE_DIR_COUNT (sizeof(sInstalledSharedCacheDirs)/sizeof(sInstalledSharedCacheDirs[0]))

// Apple maps an installed cache through a Mapper that presents the cache's own address space
// (Mapper::mapperForSharedCache() opens the file, peeks the header, and maps each of the cache's
// mappings at its declared address).  mapStartOfCache() above maps only the first megabyte, which
// is all DyldSharedCache::forEachImageTextSegment() ever reads; the introspection SPI reaches image
// content anywhere in the cache, so the whole file is mapped here -- the same view, one mapping.
static const DyldSharedCache* mapSharedCacheFile(const char* path, size_t& sizeMapped)
{
	struct stat statbuf;
	if ( dyld3::stat(path, &statbuf) == -1 )
		return NULL;
	if ( statbuf.st_size < 0x004000 )      // Apple's cacheFilePeek() reads a 16 KiB prefix
		return NULL;
	int fd = dyld3::open(path, O_RDONLY, 0);
	if ( fd < 0 )
		return NULL;
	void* mapping = ::mmap(NULL, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	if ( mapping == MAP_FAILED )
		return NULL;
	const DyldSharedCache* cache = (const DyldSharedCache*)mapping;
	// Apple's cacheFilePeek(): a file that does not begin with the cache magic is not a cache, and an
	// unrelated file that happens to sit in a cache directory is not reported.
	if ( ::strncmp(cache->header.magic, "dyld_v1", 7) != 0 ) {
		::munmap(mapping, (size_t)statbuf.st_size);
		return NULL;
	}
	sizeMapped = (size_t)statbuf.st_size;
	return cache;
}

static dyld_shared_cache_t makeFileSharedCacheHandle(const DyldSharedCache* cache, size_t sizeMapped, bool privateMapping)
{
	dyld_shared_cache_t handle = (dyld_shared_cache_t)::malloc(sizeof(struct dyld_shared_cache_s));
	if ( handle == NULL )
		return NULL;
	handle->cache          = cache;
	handle->contentBase    = (const uint8_t*)cache;
	handle->unslidBase     = cache->unslidLoadAddress();
	// Atlas::SharedCache::createForFileRecord() maps the cache file with no slide, so such a cache
	// answers at its own unslid base -- the base address the SPI documents for an installed cache.
	handle->rebasedAddress = handle->unslidBase;
	handle->mappedPrivate  = privateMapping;
	handle->sizeMapped     = sizeMapped;
	return handle;
}

static void disposeSharedCacheHandle(dyld_shared_cache_t handle)
{
	if ( handle == NULL )
		return;
	if ( handle->sizeMapped != 0 )
		::munmap((void*)handle->cache, handle->sizeMapped);
	::free(handle);
}

// Apple's mapperForSharedCache() falls back to the cache file whenever the cache in question is not
// the one this process has mapped; this is that fallback, by the same uuid rule and over the same
// directories the enumeration walks.
static dyld_shared_cache_t findInstalledSharedCacheWithUUID(const uuid_t wantedUUID)
{
	for ( size_t i = 0; i < INSTALLED_CACHE_DIR_COUNT; ++i ) {
		char dirRealPath[PATH_MAX];
		if ( realpath(sInstalledSharedCacheDirs[i], dirRealPath) == NULL )
			continue;
		DIR* dirp = ::opendir(dirRealPath);
		if ( dirp == NULL )
			continue;
		dirent entry;
		dirent* entp = NULL;
		char cachePath[PATH_MAX];
		while ( ::readdir_r(dirp, &entry, &entp) == 0 ) {
			if ( entp == NULL )
				break;
			if ( entp->d_type != DT_REG )
				continue;
			if ( isSharedCacheSubCacheLeafName(entp->d_name) )
				continue;
			if ( strlcpy(cachePath, dirRealPath, PATH_MAX ) >= PATH_MAX )
				continue;
			if ( strlcat(cachePath, entp->d_name, PATH_MAX) >= PATH_MAX )
				continue;
			size_t sizeMapped = 0;
			const DyldSharedCache* cache = mapSharedCacheFile(cachePath, sizeMapped);
			if ( cache == NULL )
				continue;
			uuid_t foundUUID;
			cache->getUUID(foundUUID);
			if ( ::memcmp(foundUUID, wantedUUID, sizeof(uuid_t)) == 0 )
				return makeFileSharedCacheHandle(cache, sizeMapped, true);
			::munmap((void*)cache, sizeMapped);
		}
		closedir(dirp);
	}
	return NULL;
}

// dyld_introspection.cpp:327, over Atlas SharedCache::forEachInstalledCacheWithSystemPath().  The
// handle is valid only for the body of the block: the cache is mapped for that body and unmapped
// again after it, exactly as Apple's ephemeral allocator owns the mapping for the block's lifetime.
//
// ON THIS GUEST THE ENUMERATION IS MEASURED EMPTY, and the empty result is reported as such: none
// of the directories above exists in the deployed prefix -- there is no dyld_shared_cache_* file
// anywhere under it -- so every realpath() fails and the loop calls the block zero times.  A
// fabricated cache, or a fabricated uuid, would be a lie; zero calls is the honest answer, and it
// is what Apple's own code also yields for a system that has no cache installed, which is what
// this guest is.
void dyld_for_each_installed_shared_cache(void (^callback)(dyld_shared_cache_t cache))
{
	char visitedRealPaths[INSTALLED_CACHE_DIR_COUNT][PATH_MAX];
	size_t visitedCount = 0;

	for ( size_t i = 0; i < INSTALLED_CACHE_DIR_COUNT; ++i ) {
		char systemCacheDirRealPath[PATH_MAX];
		if ( realpath(sInstalledSharedCacheDirs[i], systemCacheDirRealPath) == NULL )
			continue;

		// several of the directories above are symlinked views of one another: visit each real
		// directory once
		bool alreadyVisited = false;
		for ( size_t j = 0; j < visitedCount; ++j ) {
			if ( strcmp(visitedRealPaths[j], systemCacheDirRealPath) == 0 ) {
				alreadyVisited = true;
				break;
			}
		}
		if ( alreadyVisited )
			continue;
		strlcpy(visitedRealPaths[visitedCount], systemCacheDirRealPath, PATH_MAX);
		++visitedCount;

		DIR* dirp = ::opendir(systemCacheDirRealPath);
		if ( dirp == NULL )
			continue;
		dirent entry;
		dirent* entp = NULL;
		char cachePath[PATH_MAX];
		while ( ::readdir_r(dirp, &entry, &entp) == 0 ) {
			if ( entp == NULL )
				break;
			if ( entp->d_type != DT_REG )
				continue;
			if ( isSharedCacheSubCacheLeafName(entp->d_name) )
				continue;
			if ( strlcpy(cachePath, systemCacheDirRealPath, PATH_MAX) >= PATH_MAX )
				continue;
			if ( strlcat(cachePath, entp->d_name, PATH_MAX) >= PATH_MAX )
				continue;
			size_t sizeMapped = 0;
			const DyldSharedCache* cache = mapSharedCacheFile(cachePath, sizeMapped);
			if ( cache == NULL )
				continue;
			dyld_shared_cache_t handle = makeFileSharedCacheHandle(cache, 0, true);   // the enumeration owns the mapping
			if ( handle != NULL ) {
				callback(handle);
				::free(handle);          // the mapping itself is given back below
			}
			::munmap((void*)cache, sizeMapped);
		}
		closedir(dirp);
	}
}

//
// Imaged backing.  Atlas Image::ml() maps an image that is not in a cache out of the file the
// process reports for it: mapperForMachO() opens that file, and for a fat file picks the slice
// whose uuid is the image's.  A file that is gone, or whose uuid does not match, leaves the image
// without a Mach-O, and every accessor below then answers as Apple's does in that state.
//
static const dyld3::MachOFile* resolveImageMachO(dyld_image_t image)
{
	if ( image->mlResolved )
		return image->ml;
	image->mlResolved = true;
	if ( (image->path == NULL) || (image->path[0] == '\0') || !image->hasExpectedUuid )
		return NULL;
	struct stat statbuf;
	if ( dyld3::stat(image->path, &statbuf) == -1 )
		return NULL;
	if ( (size_t)statbuf.st_size < sizeof(mach_header_64) )
		return NULL;
	int fd = dyld3::open(image->path, O_RDONLY, 0);
	if ( fd < 0 )
		return NULL;
	void* mapping = ::mmap(NULL, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	::close(fd);
	if ( mapping == MAP_FAILED )
		return NULL;
	const dyld3::MachOFile* found = NULL;
	uint64_t sliceOffset = 0;
	if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(mapping) ) {
		Diagnostics diag;
		__block const dyld3::MachOFile* foundByUUID = NULL;
		__block uint64_t sliceOffsetByUUID = 0;
		ff->forEachSlice(diag, (uint64_t)statbuf.st_size, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
			const dyld3::MachOFile* slice = (const dyld3::MachOFile*)sliceStart;
			uuid_t sliceUUID;
			if ( slice->hasMachOMagic() && slice->getUuid(sliceUUID) && (::memcmp(sliceUUID, image->expectedUuid, sizeof(uuid_t)) == 0) ) {
				foundByUUID = slice;
				sliceOffsetByUUID = (uint64_t)((const uint8_t*)sliceStart - (const uint8_t*)mapping);
				stop = true;
			}
		});
		diag.clearError();
		found = foundByUUID;
		sliceOffset = sliceOffsetByUUID;
	} else {
		const dyld3::MachOFile* slice = (const dyld3::MachOFile*)mapping;
		uuid_t sliceUUID;
		Diagnostics diag;
		if ( slice->hasMachOMagic() && slice->isMachO(diag, (uint64_t)statbuf.st_size)
				&& slice->getUuid(sliceUUID) && (::memcmp(sliceUUID, image->expectedUuid, sizeof(uuid_t)) == 0) )
			found = slice;
	}
	if ( found == NULL ) {
		::munmap(mapping, (size_t)statbuf.st_size);
		return NULL;
	}
	image->ml              = found;
	image->mappedFile      = mapping;
	image->mappedFileSize  = (size_t)statbuf.st_size;
	image->fileSliceOffset = sliceOffset;
	return found;
}

// MachOAnalyzer::preferredLoadAddress(), which Atlas Image::forEachSegment() uses for the slide of
// an image that is not in a cache.
static uint64_t imagePreferredLoadAddress(const dyld3::MachOFile* mh)
{
	__block uint64_t textVmAddr = 0;
	mh->forEachSegment(^(const dyld3::MachOFile::SegmentInfo& info, bool& stop) {
		if ( ::strcmp(info.segName, "__TEXT") == 0 ) {
			textVmAddr = info.vmAddr;
			stop = true;
		}
	});
	return textVmAddr;
}

static void initImage(dyld_image_t image)
{
	::memset(image, 0, sizeof(struct dyld_image_s));
}

static void disposeImage(dyld_image_t image)
{
	if ( image->mappedFile != NULL ) {
		::munmap(image->mappedFile, image->mappedFileSize);
		image->mappedFile = NULL;
	}
	image->ml         = NULL;
	image->mlResolved = false;
}

// Atlas Image::forEachSegment().  An image from a cache is reported at its unslid address and one
// loaded in a process at its loaded address; the main executable's __PAGEZERO is skipped.
static bool forEachImageSegment(dyld_image_t image, void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm))
{
	const dyld3::MachOFile* mh = resolveImageMachO(image);
	if ( mh == NULL )
		return false;
	uint64_t slide = image->rebasedAddress - imagePreferredLoadAddress(mh);
	mh->forEachSegment(^(const dyld3::MachOFile::SegmentInfo& info, bool& stop) {
		uint64_t vmAddr = 0;
		if ( image->inSharedCache ) {
			vmAddr = info.vmAddr + image->sharedCacheSlide;
		} else {
			if ( mh->isMainExecutable() && (::strncmp(info.segName, "__PAGEZERO", 10) == 0) )
				return;
			vmAddr = info.vmAddr + slide;
		}
		block(info.segName, vmAddr, info.vmSize, (int)info.protections);
	});
	return true;
}

// Atlas Mapper::map(): the buffer backing the segment, which for an image in a cache is the cache
// mapping and for an image in a process is the on-disk file.  Apple's Mapper::Pointer is scoped to
// the contentReader call, and so is this pointer.
static const void* imageSegmentContent(dyld_image_t image, const dyld3::MachOFile::SegmentInfo& info)
{
	if ( image->inSharedCache )
		return image->contentBase + (info.vmAddr - image->contentUnslidBase);
	return (const uint8_t*)image->mappedFile + image->fileSliceOffset + info.fileOffset;
}

// Atlas Image::contentForSegment().
static bool imageContentForSegment(dyld_image_t image, const char* segmentName,
                                   void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize))
{
	const dyld3::MachOFile* mh = resolveImageMachO(image);
	if ( mh == NULL )
		return false;
	__block bool result = false;
	uint64_t slide = image->rebasedAddress - imagePreferredLoadAddress(mh);
	mh->forEachSegment(^(const dyld3::MachOFile::SegmentInfo& info, bool& stop) {
		if ( ::strcmp(segmentName, info.segName) != 0 )
			return;
		uint64_t vmAddr = 0;
		if ( image->inSharedCache ) {
			vmAddr = info.vmAddr + image->sharedCacheSlide;
		} else {
			if ( mh->isMainExecutable() && (::strncmp(info.segName, "__PAGEZERO", 10) == 0) )
				return;
			vmAddr = info.vmAddr + slide;
		}
		if ( info.vmSize )
			contentReader(imageSegmentContent(image, info), vmAddr, info.vmSize);
		else
			contentReader(NULL, vmAddr, 0);
		result = true;
		stop = true;
	});
	return result;
}

//
// Process.  dyld_introspection.cpp:105/119/161/182, over Atlas Process and ProcessSnapshot.  The
// snapshot itself is this tree's own reader of the target's dyld_all_image_infos, which is the same
// source and the same failure values Apple's newer Process::getSnapshot() reads.
//
dyld_process_t dyld_process_create_for_task(task_read_t task, kern_return_t *kr)
{
	kern_return_t krSink = KERN_SUCCESS;
	if ( kr == NULL )
		kr = &krSink;
	*kr = KERN_SUCCESS;
	// Process::Process() takes a snapshot up front and reports its error through kr; the object is
	// returned regardless and only dyld_process_snapshot_create_for_process() needs a valid one.
	dyld_process_info info = _dyld_process_info_create(task, 0, kr);
	dyld_process_t result = (dyld_process_t)::malloc(sizeof(struct dyld_process_s));
	if ( result == NULL ) {
		if ( info != NULL )
			_dyld_process_info_release(info);
		*kr = KERN_NO_SPACE;
		return NULL;
	}
	result->task = task;
	result->info = info;
	return result;
}

// dyld_introspection.cpp:119 -- UniquePtr<Process> temp((Process*)process);
void dyld_process_dispose(dyld_process_t process)
{
	if ( process == NULL )
		return;
	if ( process->info != NULL )
		_dyld_process_info_release(process->info);
	::free(process);
}

// dyld_introspection.cpp:161 -- ((Process*)process)->getSnapshot(kr).release();
dyld_process_snapshot_t dyld_process_snapshot_create_for_process(dyld_process_t process, kern_return_t *kr)
{
	kern_return_t krSink = KERN_SUCCESS;
	if ( kr == NULL )
		kr = &krSink;
	*kr = KERN_SUCCESS;
	// the guard framework/LegacyAPIShims.mm carries for the non-Atlas entry point
	if ( process == NULL ) {
		*kr = KERN_FAILURE;
		return NULL;
	}
	dyld_process_info info = _dyld_process_info_create(process->task, 0, kr);
	if ( info == NULL )
		return NULL;
	dyld_process_snapshot_t snapshot = (dyld_process_snapshot_t)::malloc(sizeof(struct dyld_process_snapshot_s));
	if ( snapshot == NULL ) {
		_dyld_process_info_release(info);
		*kr = KERN_NO_SPACE;
		return NULL;
	}
	snapshot->info          = info;
	snapshot->cache         = NULL;
	snapshot->cacheResolved = false;
	return snapshot;
}

// dyld_introspection.cpp:182 -- UniquePtr<ProcessSnapshot> temp(processSnapshot);
void dyld_process_snapshot_dispose(dyld_process_snapshot_t snapshot)
{
	if ( snapshot == NULL )
		return;
	disposeSharedCacheHandle(snapshot->cache);
	if ( snapshot->info != NULL )
		_dyld_process_info_release(snapshot->info);
	::free(snapshot);
}

// dyld_introspection.cpp:192, over ProcessSnapshot::forEachImage(): every image loaded in the
// process.  Apple's Atlas snapshot adds the shared-cache images the process has loaded from the
// cache it maps; this tree's reader lists every image in the target's dyld_all_image_infos,
// cache-resident ones included, and this guest maps no cache at all, so nothing is elided.
void dyld_process_snapshot_for_each_image(dyld_process_snapshot_t snapshot, void (^block)(dyld_image_t image))
{
	if ( (snapshot == NULL) || (block == NULL) )
		return;
	if ( snapshot->info == NULL )    // Apple: a snapshot that is not valid iterates nothing
		return;
	_dyld_process_info_for_each_image(snapshot->info, ^(uint64_t machHeaderAddress, const uuid_t uuid, const char* path) {
		struct dyld_image_s image;
		initImage(&image);
		image.rebasedAddress = machHeaderAddress;
		image.path = ((path != NULL) && (path[0] != '\0')) ? path : NULL;
		if ( uuid != NULL ) {
			::memcpy(image.expectedUuid, uuid, sizeof(uuid_t));
			for ( int i = 0; i < 16; ++i ) {
				if ( uuid[i] != 0 ) {
					image.hasExpectedUuid = true;
					break;
				}
			}
		}
		block(&image);
		disposeImage(&image);
	});
}

// dyld_introspection.cpp:204 -- processSnapshot->sharedCache().  Apple's Atlas builds it with
// mapperForSharedCache(): the cache this process has mapped when the uuid is that one, otherwise
// the cache file.  A process with no cache at all answers NULL, which is this guest's answer too.
dyld_shared_cache_t dyld_process_snapshot_get_shared_cache(dyld_process_snapshot_t snapshot)
{
	if ( (snapshot == NULL) || (snapshot->info == NULL) )
		return NULL;
	if ( !snapshot->cacheResolved ) {
		snapshot->cacheResolved = true;
		dyld_process_cache_info cacheInfo;
		_dyld_process_info_get_cache(snapshot->info, &cacheInfo);
		if ( !cacheInfo.noCache ) {
			const DyldSharedCache* localCache = (const DyldSharedCache*)dyld3::gAllImages.cacheLoadAddress();
			if ( localCache != NULL ) {
				uuid_t localUUID;
				localCache->getUUID(localUUID);
				if ( ::memcmp(localUUID, cacheInfo.cacheUUID, sizeof(uuid_t)) == 0 ) {
					dyld_shared_cache_t handle = (dyld_shared_cache_t)::malloc(sizeof(struct dyld_shared_cache_s));
					if ( handle != NULL ) {
						handle->cache          = localCache;
						handle->contentBase    = (const uint8_t*)localCache;
						handle->unslidBase     = localCache->unslidLoadAddress();
						handle->rebasedAddress = (uint64_t)localCache;
						handle->mappedPrivate  = cacheInfo.privateCache;
						handle->sizeMapped     = 0;   // this mapping belongs to the process, not us
						snapshot->cache = handle;
					}
				}
			}
			if ( snapshot->cache == NULL )
				snapshot->cache = findInstalledSharedCacheWithUUID(cacheInfo.cacheUUID);
		}
	}
	return snapshot->cache;
}

//
// Shared cache.  dyld_introspection.cpp:239-300, over Atlas SharedCache.
//
uint64_t dyld_shared_cache_get_base_address(dyld_shared_cache_t cache_atlas)
{
	if ( cache_atlas == NULL )
		return 0;
	return cache_atlas->rebasedAddress;
}

uint64_t dyld_shared_cache_get_mapped_size(dyld_shared_cache_t cache_atlas)
{
	if ( cache_atlas == NULL )
		return 0;
	// Atlas SharedCache::size(), which is the same computation as DyldSharedCache::mappedSize()
	return cache_atlas->cache->mappedSize();
}

bool dyld_shared_cache_is_mapped_private(dyld_shared_cache_t cache_atlas)
{
	if ( cache_atlas == NULL )
		return false;
	return cache_atlas->mappedPrivate;
}

void dyld_shared_cache_copy_uuid(dyld_shared_cache_t cache_atlas, uuid_t *uuid)
{
	if ( (cache_atlas == NULL) || (uuid == NULL) )
		return;
	// Atlas: memcpy(&uuid[0], cache->uuid().begin(), 16)
	cache_atlas->cache->getUUID(*uuid);
}

void dyld_shared_cache_for_each_image(dyld_shared_cache_t cache, void (^block)(dyld_image_t image))
{
	if ( (cache == NULL) || (block == NULL) )
		return;
	// Atlas SharedCache::forEachImage(): one Image per dyld_cache_image_text_info, valid for the body
	// of the block, whose content the cache's own mapping backs and whose filename() is NULL.
	cache->cache->forEachImage(^(const mach_header* mh, const char* installName) {
		struct dyld_image_s image;
		initImage(&image);
		image.ml                = (const dyld3::MachOFile*)mh;
		image.mlResolved        = true;
		image.inSharedCache     = true;
		image.sharedCacheSlide  = cache->rebasedAddress - cache->unslidBase;
		image.rebasedAddress    = cache->unslidBase + ((const uint8_t*)mh - cache->contentBase);
		image.contentBase       = cache->contentBase;
		image.contentUnslidBase = cache->unslidBase;
		block(&image);
		disposeImage(&image);
	});
}

//
// Image.  dyld_introspection.cpp:354-475, over Atlas Image.
//
bool dyld_image_content_for_segment(dyld_image_t image, const char* segmentName,
                                    void (^contentReader)(const void* content, uint64_t vmAddr, uint64_t vmSize))
{
	if ( (image == NULL) || (segmentName == NULL) || (contentReader == NULL) )
		return false;
	return imageContentForSegment(image, segmentName, contentReader);
}

bool dyld_image_copy_uuid(dyld_image_t image, uuid_t* uuid)
{
	if ( (image == NULL) || (uuid == NULL) )
		return false;
	const dyld3::MachOFile* mh = resolveImageMachO(image);
	if ( mh == NULL )
		return false;
	uuid_t imageUUID;
	if ( !mh->getUuid(imageUUID) )    // Atlas: an image with an empty uuid answers false
		return false;
	::memcpy((void*)&uuid[0], imageUUID, sizeof(uuid_t));
	return true;
}

bool dyld_image_for_each_segment_info(dyld_image_t image,
                                      void (^block)(const char* segmentName, uint64_t vmAddr, uint64_t vmSize, int perm))
{
	if ( (image == NULL) || (block == NULL) )
		return false;
	return forEachImageSegment(image, block);
}

const char* dyld_image_get_installname(dyld_image_t image)
{
	if ( image == NULL )
		return NULL;
	const dyld3::MachOFile* mh = resolveImageMachO(image);
	if ( mh == NULL )
		return NULL;
	return mh->installName();
}

const char* dyld_image_get_file_path(dyld_image_t image)
{
	if ( image == NULL )
		return NULL;
	// Atlas Image::filename(): NULL for an image that is only in the shared cache, the backing file
	// path otherwise.
	if ( image->inSharedCache )
		return NULL;
	if ( (image->path == NULL) || (image->path[0] == '\0') )
		return NULL;
	return image->path;
}

bool _dyld_is_memory_immutable(const void* addr, size_t length)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_is_memory_immutable(addr, length);

	DYLD_NO_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(const void*, size_t);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_is_memory_immutable", &p);
	return p(addr, length);
}


void _dyld_objc_notify_register(_dyld_objc_notify_mapped    mapped,
                                _dyld_objc_notify_init      init,
                                _dyld_objc_notify_unmapped  unmapped)
{
	if ( gUseDyld3 )
		return dyld3::_dyld_objc_notify_register(mapped, init, unmapped);

	DYLD_LOCK_THIS_BLOCK;
    typedef bool (*funcType)(_dyld_objc_notify_mapped, _dyld_objc_notify_init, _dyld_objc_notify_unmapped);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

	if(p == NULL)
	    dyld_func_lookup_and_resign("__dyld_objc_notify_register", &p);
	p(mapped, init, unmapped);
}

void _dyld_missing_symbol_abort()
{
    return dyld3::_dyld_missing_symbol_abort();
}

const char* _dyld_get_objc_selector(const char* selName)
{
    // Check the shared cache table if it exists.
    if ( gObjCOpt != nullptr ) {
        if ( const objc_opt::objc_selopt_t* selopt = gObjCOpt->selopt() ) {
            const char* name = selopt->get(selName);
            if (name != nullptr)
                return name;
        }
    }

    if ( gUseDyld3 )
        return dyld3::_dyld_get_objc_selector(selName);

    return nullptr;
}

void _dyld_for_each_objc_class(const char* className,
                           void (^callback)(void* classPtr, bool isLoaded, bool* stop)) {
    if ( gUseDyld3 )
        return dyld3::_dyld_for_each_objc_class(className, callback);
}

void _dyld_for_each_objc_protocol(const char* protocolName,
                                  void (^callback)(void* protocolPtr, bool isLoaded, bool* stop)) {
    if ( gUseDyld3 )
        return dyld3::_dyld_for_each_objc_protocol(protocolName, callback);
}

void _dyld_register_driverkit_main(void (*mainFunc)(void))
{
    if ( gUseDyld3 )
        return dyld3::_dyld_register_driverkit_main(mainFunc);

    typedef bool (*funcType)(void *);
    static funcType __ptrauth_dyld_function_ptr p = NULL;

    if(p == NULL)
        dyld_func_lookup_and_resign("__dyld_register_driverkit_main", &p);
    p(reinterpret_cast<void *>(mainFunc));
}

// This is populated in the shared cache builder, so that the ranges are protected by __DATA_CONST
// If we have a root, we can find this range in the shared cache libdyld at runtime
typedef std::pair<const uint8_t*, const uint8_t*> ObjCConstantRange;

#if TARGET_OS_OSX
__attribute__((section(("__DATA, __objc_ranges"))))
#else
__attribute__((section(("__DATA_CONST, __objc_ranges"))))
#endif
__attribute__((used))
static ObjCConstantRange gSharedCacheObjCConstantRanges[dyld_objc_string_kind + 1];

static std::pair<const void*, uint64_t> getDyldCacheConstantRanges() {
    const dyld_all_image_infos* allInfo = _dyld_get_all_image_infos();
    if ( allInfo != nullptr  ) {
        const DyldSharedCache* cache = (const DyldSharedCache*)(allInfo->sharedCacheBaseAddress);
        if ( cache != nullptr ) {
            return cache->getObjCConstantRange();
        }
    }
    return { nullptr, 0 };
}

bool _dyld_is_objc_constant(DyldObjCConstantKind kind, const void* addr) {
    assert(kind <= dyld_objc_string_kind);
    // The common case should be that the value is in range, as this is a security
    // check, so first test against the values in the struct.  If we have a root then
    // we'll take the slow path later
    if ( (addr >= gSharedCacheObjCConstantRanges[kind].first) && (addr < gSharedCacheObjCConstantRanges[kind].second) ) {
        // Make sure that we are pointing at the start of a constant object, not in to the middle of it
        uint64_t offset = (uint64_t)addr - (uint64_t)gSharedCacheObjCConstantRanges[kind].first;
        return (offset % (uint64_t)DyldSharedCache::ConstantClasses::cfStringAtomSize) == 0;
    }

    // If we are in the shared cache, then the above check was sufficient, so this really isn't a valid constant address
    extern void* __dso_handle;
    const dyld3::MachOAnalyzer* ma = (const dyld3::MachOAnalyzer*)&__dso_handle;
    if ( ma->inDyldCache() )
        return false;

    // We now know we are a root, so use the pointers in the shared cache libdyld version of gSharedCacheObjCConstantRanges
    static std::pair<const void*, uint64_t> sharedCacheRanges = { nullptr, ~0ULL };

    // FIXME: Should we fold this in as an inititalizer above?
    // That would mean we need to link against somewhere to get ___cxa_guard_acquire/___cxa_guard_release
    if ( sharedCacheRanges.second == ~0ULL )
        sharedCacheRanges = getDyldCacheConstantRanges();

    // We have the range of the section in libdyld in the shared cache, now get an array of ranges from it
    uint64_t numRanges = sharedCacheRanges.second / sizeof(ObjCConstantRange);
    if ( kind >= numRanges )
        return false;

    const ObjCConstantRange* rangeArrayBase = (const ObjCConstantRange*)sharedCacheRanges.first;
    if ( (addr >= rangeArrayBase[kind].first) && (addr < rangeArrayBase[kind].second) ) {
        // Make sure that we are pointing at the start of a constant object, not in to the middle of it
        uint64_t offset = (uint64_t)addr - (uint64_t)rangeArrayBase[kind].first;
        return (offset % (uint64_t)DyldSharedCache::ConstantClasses::cfStringAtomSize) == 0;
    }
    return false;
}


//
// Apple's macho_* Mach-O SPI: the architecture-name lookup
// =======================================================
//
// PORTED, not re-derived, from the dyld-1125.5 tag -- the generation the macOS 14 guest reports in
// /System/Library/CoreServices/SystemVersion.plist (14.0, 23A344) and the one the SymbolicationDT
// that binds this name was built against:
//
//   libdyld/utils.cpp:56        macho_cpu_type_for_arch_name()
//   common/MachOFile.cpp:566    MachOFile::cpuTypeFromArchName()
//   common/MachOFile.cpp:476    MachOFile::_s_archInfos[]
//   include/mach-o/utils.h:59   the documented contract (macOS 13.0+)
//
// WHY IT IS NEEDED.  SymbolicationDT's chained-fixups import table binds _macho_cpu_type_for_arch_name
// STRONG and EAGER on the libSystem ordinal (this tree's /usr/lib/libSystem.B.dylib re-exports
// libdyld.dylib), so the loader refuses the whole image until it resolves, whether or not it is ever
// called.  Measured from the image's own bytes rather than counted by hand
// (llvm-objdump --macho --arch=x86_64 --chained-fixups on that framework): 662 imports, of which
// this one is the ONLY macho_* name, bound strong (weak_import 0) to lib_ordinal 13 =
// /usr/lib/libSystem.B.dylib.  Its one call site in the image is 0x37ba3, which feeds it a name and
// two out parameters.
//
// THE CONTRACT, Apple's and unchanged: a name that is known fills in type/subtype and returns true;
// a name that is UNKNOWN returns false and leaves the two out parameters UNTOUCHED.  The miss path
// not writing is the validation this port must carry -- the call site pre-zeroes the two 32-bit
// slots (xorl/movl at 0x37b3c-0x37b41) and branches on %al (testb/je at 0x37ba8-0x37baa), so a
// made-up type written on the miss path would silently turn its "unknown architecture" branch into
// a false positive.  Apple's loop writes only inside the match.
//
// WHAT THIS GUEST CANNOT DO: nothing here needs a shared cache, a mapped file or a task -- the
// answer is a table lookup.  The one honest gap is the table's own extent: Apple guards its arm64e
// and arm64_32 entries with SUPPORT_ARCH_arm64e / SUPPORT_ARCH_arm64_32, and this tree defines
// neither (no definition anywhere in the dyld sources, in the generated CMake text or in
// build/verify14/build.ninja), so "arm64e" gets the answer Apple's own implementation gives when
// that macro is 0: false, outputs untouched.  That is stated here rather than repaired by inventing
// a table entry Apple's implementation does not have in this build.
//
// CORRECTION (the rung that added the inverse below): the sentence that used to stand here said
// "the ported table is the same nine-entry set the tree's own dyld3/MachOFile.cpp carries".  It is
// NOT, and a reader who wired the inverse to MachOFile::archName() on the strength of it would
// silently lose four architectures: read with the guards as this build resolves them, the table in
// this file carries ELEVEN entries (x86_64, x86_64h, i386, arm64, armv7k, armv7s, armv7, armv6m,
// armv7m, armv7em, armv8m) while dyld3/MachOFile.cpp's _s_archInfos[] carries SEVEN -- the four
// armv6m/armv7m/armv7em/armv8m rows exist here (they are rows of the dyld-1125.5 table this block is
// ported from) and not there.  That difference is why the inverse below reads THIS table: one name's
// two directions must not answer two different sets, and the compile-time check beside the table is
// what keeps that true.
//
// The rest of the family in libdyld/utils.cpp -- macho_arch_name_for_cpu_type(),
// macho_arch_name_for_mach_header(), macho_for_each_slice(), macho_for_each_slice_in_fd(),
// macho_best_slice(), macho_best_slice_in_fd(), macho_dylib_install_name() -- is NOT implemented,
// with TWO exceptions, one per rung: macho_for_each_slice() (ported below, for
// DVTInstrumentsFoundation) and macho_arch_name_for_cpu_type() (ported below that, for
// DVTFoundation).
//
// CORRECTION: the sentence that used to stand here said "no image measured in this ladder's closure
// imports any of them (SymbolicationDT's 662 imports contain exactly one macho_* name, the one
// below; CoreSymbolicationDT and MallocStackLoggingDT import none)".  That measurement HAS NOW
// CHANGED and the sentence was false, so it is corrected rather than kept.  Re-measured 2026-10-12
// from the coverage graph entered at the guest's own run log (closure_analyze.py --cover, 254
// reached images, every import decoded from each image's own chained-fixups/bind bytes): the
// closure now contains THREE macho_* importers, not one, and every name below is bound STRONG and
// EAGER on the libSystem ordinal (field /usr/lib/libSystem.B.dylib) --
//
//   DVTInstrumentsFoundation (1141 imports)   _macho_for_each_slice                  <- ported below
//   XCTHarness                                _macho_for_each_slice
//   XCTHarness                                _macho_arch_name_for_cpu_type
//   XCTHarness                                _macho_arch_name_for_mach_header
//   DVTFoundation                             _macho_arch_name_for_cpu_type
//   AppThinning                               _macho_arch_name_for_cpu_type
//
// Ported so far, and only these: macho_for_each_slice() (the rung where DVTInstrumentsFoundation's
// stop demanded it) and macho_arch_name_for_cpu_type() (THIS rung: the ladder's stop is
// DVTFoundation's own __dyld_get_dlopen_image_header, and the same image binds
// _macho_arch_name_for_cpu_type strong and eager beside it -- chained import 601 of 2261 on
// lib_ordinal 3).  macho_arch_name_for_mach_header() is NOT ported, and neither are
// macho_for_each_slice_in_fd(), macho_best_slice(), macho_best_slice_in_fd() or
// macho_dylib_install_name(): the images that demand the mach_header form (XCTHarness) load PAST
// this stop, and porting an entry point no reached image imports would be inventing surface, not
// answering one.  SymbolicationDT is still in the closure and still carries exactly one macho_*
// name, the cpu-type lookup above (662 imports, verified again in this rung's re-measurement).
//

// Apple's MachOFile::_s_archInfos[] (common/MachOFile.cpp, dyld-1125.5), verbatim -- including its
// guards on the arm64e and arm64_32 entries, which resolve to nothing in this tree (see above).
struct MachOArchInfo {
	const char* name;
	uint32_t    cputype;
	uint32_t    cpusubtype;
};

// constexpr, not merely const, so the round-trip check below can be a compile-time assertion.
static constexpr MachOArchInfo sMachOArchInfos[] = {
	{ "x86_64",   CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_ALL  },
	{ "x86_64h",  CPU_TYPE_X86_64,   CPU_SUBTYPE_X86_64_H    },
	{ "i386",     CPU_TYPE_I386,     CPU_SUBTYPE_I386_ALL    },
	{ "arm64",    CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64_ALL   },
#if SUPPORT_ARCH_arm64e
	{ "arm64e",   CPU_TYPE_ARM64,    CPU_SUBTYPE_ARM64E      },
#endif
#if SUPPORT_ARCH_arm64_32
	{ "arm64_32", CPU_TYPE_ARM64_32, CPU_SUBTYPE_ARM64_32_V8 },
#endif
	{ "armv7k",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7K     },
	{ "armv7s",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7S     },
	{ "armv7",    CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7      },
	{ "armv6m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V6M     },
	{ "armv7m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7M     },
	{ "armv7em",  CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V7EM    },
	{ "armv8m",   CPU_TYPE_ARM,      CPU_SUBTYPE_ARM_V8M     },
};

// Apple's MachOFile::cpuTypeFromArchName() (common/MachOFile.cpp:566, dyld-1125.5).  The two
// out parameters are assigned only on the match, which is what makes the miss path leave them as
// the caller left them.
static bool machoCPUTypeFromArchName(const char* archName, cpu_type_t* cputype, cpu_subtype_t* cpusubtype)
{
	for (const MachOArchInfo& info : sMachOArchInfos) {
		if ( strcmp(archName, info.name) == 0 ) {
			*cputype    = info.cputype;
			*cpusubtype = info.cpusubtype;
			return true;
		}
	}
	return false;
}

extern "C" {
bool macho_cpu_type_for_arch_name(const char* archName, cpu_type_t* type, cpu_subtype_t* subtype);
}

// Apple's macho_cpu_type_for_arch_name() (libdyld/utils.cpp:56, dyld-1125.5) is nothing but that
// lookup: it declares both out parameters _Nonnull and answers false without writing them.
bool macho_cpu_type_for_arch_name(const char* archName, cpu_type_t* type, cpu_subtype_t* subtype)
{
	return machoCPUTypeFromArchName(archName, type, subtype);
}


//
// Apple's macho_* Mach-O SPI: the inverse lookup, and the two directions' agreement
// ================================================================================
//
// PORTED, not re-derived, from the same dyld-1125.5 tag:
//
//   libdyld/utils.cpp:65        macho_arch_name_for_cpu_type()
//   common/MachOFile.cpp:556    MachOFile::archName()
//   include/mach-o/utils.h:52   the documented contract (macOS 13.0+)
//
// (The name is SPI in libdyld, NOT in libmacho: the SDK's own
// .../MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib/system/libdyld.tbd lists
// _macho_arch_name_for_cpu_type in its export set, while libmacho.tbd next to it lists only the
// NX*/getsect* family.  So the inverse belongs in the same file as macho_cpu_type_for_arch_name and
// macho_for_each_slice above -- the tree's libmacho (src/external/cctools) is the cctools arch
// table, a different image with a different table, and answering from there would be a second
// convention for one name.)
//
// WHY IT IS NEEDED.  DVTFoundation's chained-fixups import table binds _macho_arch_name_for_cpu_type
// STRONG and EAGER on the libSystem ordinal (measured from the image's own bytes: chained import
// 601 of 2261, lib_ordinal 3 = /usr/lib/libSystem.B.dylib, weak_import 0), so the loader refuses the
// whole image until it resolves.  Its ONE call site is 0x161bad, in
// ___DVTMachOArchitecturesForExecutable_block_invoke, with the fat slice's own cputype/cpusubtype
// from the loop (movl -0x2c(%rbp),%edi; movl %r12d,%esi); the image nil-tests the returned pointer
// (testq %rax,%rax; je -> the _unknownArchError path) and otherwise builds an NSString from the C
// string with NSUTF8StringEncoding (movl $0x4,%ecx; %rdx = the returned name).  NULL is therefore the
// honest miss and a made-up name would be a silent wrong answer.
//
// THE TWO DIRECTIONS MUST AGREE.  Matching is not symmetric -- the name side compares the name
// exactly and the type side masks the ABI bits off the subtype -- so name -> pair -> name holds only
// when (a) no stored subtype has ABI bits that the type side would mask away, and (b) no two entries
// match the same pair.  Both are properties of the table alone and are asserted here, so an edit
// that broke the inverse fails this build instead of answering the two callers differently:
//
constexpr bool machoArchInfosRoundTrip()
{
	const size_t count = sizeof(sMachOArchInfos) / sizeof(sMachOArchInfos[0]);
	for (size_t i = 0; i < count; ++i) {
		// (a) the pair the name side hands out must survive the type side's masking unchanged
		if ( (sMachOArchInfos[i].cpusubtype & ~CPU_SUBTYPE_MASK) != sMachOArchInfos[i].cpusubtype )
			return false;
		for (size_t j = i + 1; j < count; ++j) {
			// (b) two names must never answer the same pair, or the inverse could pick the other one
			if ( (sMachOArchInfos[i].cputype == sMachOArchInfos[j].cputype)
			  && (sMachOArchInfos[i].cpusubtype == (sMachOArchInfos[j].cpusubtype & ~CPU_SUBTYPE_MASK)) )
				return false;
		}
	}
	return true;
}
static_assert(machoArchInfosRoundTrip(), "the arch-name table's two directions would disagree");

// Apple's MachOFile::archName() (common/MachOFile.cpp:556, dyld-1125.5), over THIS file's table --
// which is what makes the agreement above a property of the build rather than of inspection.  It
// answers Apple's sentinel "unknown" on the miss (the wrapper below translates that into NULL) and
// masks the ABI bits off the passed subtype, which is the reason a fat slice carrying
// CPU_SUBTYPE_LIB64 (0x80000003 for x86_64) still answers "x86_64".
static const char* machoArchNameForCpuType(cpu_type_t cputype, cpu_subtype_t cpusubtype)
{
	for (const MachOArchInfo& info : sMachOArchInfos) {
		if ( (cputype == (cpu_type_t)info.cputype) && (((uint32_t)cpusubtype & ~CPU_SUBTYPE_MASK) == info.cpusubtype) )
			return info.name;
	}
	return "unknown";
}

extern "C" {
const char* macho_arch_name_for_cpu_type(cpu_type_t type, cpu_subtype_t subtype);
}

// Apple's macho_arch_name_for_cpu_type() (libdyld/utils.cpp:65, dyld-1125.5): the lookup above with
// the sentinel translated, so everything the table cannot answer -- an unknown name, and a pair its
// guards exclude, such as arm64e in this configuration -- answers NULL.
//
// WHAT THIS GUEST CANNOT DO: nothing.  The answer is the table lookup above; it needs no shared
// cache, no mapped file and no task, and the table's extent is this file's, whose two directions are
// asserted consistent at compile time.  macho_arch_name_for_mach_header(), the third name in Apple's
// trio, is NOT implemented: XCTHarness imports it, XCTHarness is past this rung's stop, and porting
// an entry point no reached image demands would be inventing surface.
const char* macho_arch_name_for_cpu_type(cpu_type_t type, cpu_subtype_t subtype)
{
	const char* result = machoArchNameForCpuType(type, subtype);
	if ( strcmp(result, "unknown") == 0 )
		return NULL;
	return result;
}


//
// Apple's macho_* Mach-O SPI: iterating the slices of a file
// =========================================================
//
// PORTED from the dyld-1125.5 tag, the same generation the cpu-type lookup above came from:
//
//   libdyld/utils.cpp:82      macho_for_each_slice()
//   libdyld/utils.cpp:93      macho_for_each_slice_in_fd()   (the body, made private here)
//   common/MachOFile.cpp:122  FatFile::isFatFile()           (this tree's dyld3/MachOFile.cpp:88)
//   common/MachOFile.cpp:160  FatFile::forEachSlice()        (this tree's dyld3/MachOFile.cpp:122)
//   common/MachOFile.cpp:988  MachOFile::isMachO()           (this tree's dyld3/MachOFile.cpp:737)
//   include/mach-o/utils.h:111  the documented contract (macOS 13.0+)
//
// WHY IT IS NEEDED.  DVTInstrumentsFoundation's chained-fixups import table binds
// _macho_for_each_slice STRONG and EAGER on the libSystem ordinal (measured from the image's own
// 1141-import table: lib_ordinal 33 = /usr/lib/libSystem.B.dylib, weak_import 0), so the loader
// refuses the whole image until it resolves, whether or not it is ever called -- this is the rung
// the coverage graph's predicted stop 6 names.  The image has TWO call sites of it (0x7c884 in the
// 0x7c8xx cluster and the XCTHarness/XCTestCore path), both passing a path and a block and treating
// a non-zero return as an errno for strerror().
//
// THE CONTRACT, Apple's and unchanged: open() the path, mmap() it, and call the block once per
// slice -- once with the whole mapping when the file is a thin Mach-O, once per architecture (in
// fat-header order) when it is universal; the block's *stop halts the walk.  Return 0 on success,
// else an errno value: ENOENT/EACCES from open, errno from fstat/mmap, EFTYPE when it is neither a
// fat nor a Mach-O file, EBADMACHO when it is fat but malformed.  A NULL block is allowed and turns
// the call into a "is this a mach-o or fat file?" predicate.
//
// WHAT THIS GUEST CAN DO: everything the contract asks.  The walk is pure file parsing over the
// tree's own dyld3::FatFile / dyld3::MachOFile (the very classes dyld3/MachOFile.cpp compiles into
// this same libdyld and that resolveImageMachO() above already drives), needs no shared cache and no
// task, and answers from the bytes it maps.  Unlike the cpu-type lookup there is no table whose
// extent this configuration truncates, so nothing about the ported body is conditionally compiled.
//
// Only this ONE entry point is ported.  macho_for_each_slice_in_fd() is a second exported name in
// Apple's libdyld, but no image in this ladder's closure imports it, so the fd body is a static
// helper here (not a symbol) and no second public surface is invented.

extern "C" {
int macho_for_each_slice(const char* path, void (^callback)(const struct mach_header* slice, uint64_t fileOffset, size_t size, bool* stop));
}

// Apple's macho_for_each_slice_in_fd() (libdyld/utils.cpp:93, dyld-1125.5), verbatim except that it
// is static here (see above).  `callback` is Apple's block, whose stop parameter is a bool* rather
// than the bool& FatFile::forEachSlice() hands its block; the bridge below takes the address of the
// reference, which is what lets a stop set inside the loop end the walk.
static int machoForEachSliceInFd(int fd, void (^callback)(const struct mach_header* slice, uint64_t fileOffset, size_t size, bool* stop))
{
	struct stat statbuf;
	if ( ::fstat(fd, &statbuf) == -1 )
		return errno;

	const void* mappedFile = ::mmap(NULL, (size_t)statbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
	if ( mappedFile == MAP_FAILED )
		return errno;

	int result = 0;
	Diagnostics diag;
	if ( const dyld3::FatFile* ff = dyld3::FatFile::isFatFile(mappedFile) ) {
		ff->forEachSlice(diag, (uint64_t)statbuf.st_size, ^(uint32_t sliceCpuType, uint32_t sliceCpuSubType, const void* sliceStart, uint64_t sliceSize, bool& stop) {
			size_t sliceOffset = (size_t)((const uint8_t*)sliceStart - (const uint8_t*)mappedFile);
			if ( callback )
				callback((const struct mach_header*)sliceStart, (uint64_t)sliceOffset, (size_t)sliceSize, &stop);
		});
		if ( diag.hasError() )
			result = EBADMACHO;
	}
	else if ( ((const dyld3::MachOFile*)mappedFile)->isMachO(diag, (uint64_t)statbuf.st_size) ) {
		bool stop = false;
		if ( callback )
			callback((const struct mach_header*)mappedFile, 0, (size_t)statbuf.st_size, &stop);
	}
	else {
		// not a fat file nor a mach-o file
		result = EFTYPE;
	}

	::munmap((void*)mappedFile, (size_t)statbuf.st_size);
	return result;
}

// Apple's macho_for_each_slice() (libdyld/utils.cpp:82, dyld-1125.5): the path-taking half is
// nothing but open() + the fd body + close(), with open()'s errno passed straight through.
int macho_for_each_slice(const char* path, void (^callback)(const struct mach_header* slice, uint64_t fileOffset, size_t size, bool* stop))
{
	int fd = dyld3::open(path, O_RDONLY, 0);
	if ( fd == -1 )
		return errno;

	int result = machoForEachSliceInFd(fd, callback);
	::close(fd);
	return result;
}
