/*
 * DirectHW.c - userspace part for DirectHW
 *
 * Copyright © 2008-2010 coresystems GmbH <info@coresystems.de>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "MacOSMacros.h"
#include "DirectHW.h"
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

/* define DEBUG to print Framework debugging information */
#undef DEBUG

#ifndef err_get_system
#define err_get_system(err) (((err)>>26)&0x3f)
#endif

#ifndef err_get_sub
#define err_get_sub(err)    (((err)>>14)&0xfff)
#endif

#ifndef err_get_code
#define err_get_code(err)   ((err)&0x3fff)
#endif

enum {
    kReadIO,
    kWriteIO,
    kPrepareMap,
    kReadMSR,
    kWriteMSR,
    kReadCpuId,
    kReadMem,
    kRead,
    kWrite,
    kNumberOfMethods
};

typedef struct {
    UInt32 offset;
    UInt32 width;
    UInt32 data; // this field is 1 or 2 or 4 bytes starting at the lowest address
} iomem_t;

typedef struct {
    UInt64 offset;
    UInt64 width;
    UInt64 data; // this field is 1 or 2 or 4 or 8 bytes starting at the lowest address
} iomem64_t;

typedef struct {
    UInt64 addr;
    UInt64 size;
} map_t;

typedef struct {
    UInt32 core;
    UInt32 index;

    union {
        uint64_t io64;

        struct {
#ifdef __BIG_ENDIAN__
            UInt32 hi;
            UInt32 lo;
#else
            UInt32 lo;
            UInt32 hi;
#endif
        } io32;
    } val;
} msrcmd_t;

typedef struct {
    uint32_t core;
    uint32_t eax;
    uint32_t ecx;
    uint32_t cpudata[4];
} cpuid_t;

typedef struct {
    uint32_t core;
    uint64_t addr;
    uint32_t data;
} readmem_t;

static io_connect_t connect = -1;
static io_service_t iokit_uc;

static int darwin_init(void)
{
    kern_return_t err;

    /* Note the actual security happens in the kernel module.
     * This check is just candy to be able to get nicer output
     */
    if (getuid() != 0) {
        /* Fun's reserved for root */
        errno = EPERM;
        return -1;
    }

    /* Get the DirectHW driver service */
    iokit_uc = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("DirectHWService"));

    if (!iokit_uc) {
        printf("DirectHW.kext not loaded.\n");
        errno = ENOSYS;
        return -1;
    }

    /* Create an instance */
    err = IOServiceOpen(iokit_uc, mach_task_self(), 0, &connect);

    /* Should not go further if error with service open */
    if (err != KERN_SUCCESS) {
        printf("Could not create DirectHW instance.\n");
        errno = ENOSYS;
        return -1;
    }

    return 0;
}

static void darwin_cleanup(void)
{
    IOServiceClose(connect);
}

kern_return_t MyIOConnectCallStructMethod(
    io_connect_t    connect,
    unsigned int    index,
    void *          in,
    size_t          dataInLen,
    void *          out,
    size_t *        dataOutLen
)
{
    kern_return_t err;
#if MAC_OS_X_VERSION_MAX_ALLOWED <= MAC_OS_X_VERSION_10_4 || MAC_OS_X_VERSION_SDK <= MAC_OS_X_VERSION_10_4
    IOByteCount dataOutCount;
    err = IOConnectMethodStructureIStructureO(connect, index, (IOItemCount)dataInLen, &dataOutCount, in, out);
    if (dataOutLen)
        *dataOutLen = dataOutCount;
#elif defined(__LP64__)
    err = IOConnectCallStructMethod(connect, index, in, dataInLen, out, dataOutLen);
#else
    if (IOConnectCallStructMethod != NULL) {
        /* OSX 10.5 or newer API is available */
        err = IOConnectCallStructMethod(connect, index, in, dataInLen, out, dataOutLen);
    }
    else {
        /* Use old API (not available for x86_64) */
        IOByteCount dataOutCount;
        err = IOConnectMethodStructureIStructureO(connect, index, (IOItemCount)dataInLen, &dataOutCount, in, out);
        if (dataOutLen)
            *dataOutLen = dataOutCount;
    }
#endif
    return err;
}

int darwin_ioread(int pos, unsigned char * buf, int len)
{
    kern_return_t err;
    size_t dataInLen;
    size_t dataOutLen;
    void *in;
    void *out;
    iomem_t in32;
    iomem_t out32;
    iomem64_t in64;
    iomem64_t out64;
    UInt64 tmpdata64;
    UInt32 tmpdata;

    if (len <= 4) {
        in = &in32;
        out = &out32;
        dataInLen = sizeof(in32);
        dataOutLen = sizeof(out32);
        in32.width = len;
        in32.offset = pos;
    }
    else if (len <= 8) {
        in = &in64;
        out = &out64;
        dataInLen = sizeof(in64);
        dataOutLen = sizeof(out64);
        in64.width = len;
        in64.offset = pos;
    }
    else {
        return 1;
    }

    err = MyIOConnectCallStructMethod(connect, kReadIO, in, dataInLen, out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return 1;

    if (len <= 4) {
        tmpdata = out32.data;
        switch (len) {
            case 1: memcpy(buf, &tmpdata, 1); break;
            case 2: memcpy(buf, &tmpdata, 2); break;
            case 4: memcpy(buf, &tmpdata, 4); break;
            case 8: memcpy(buf, &tmpdata, 8); break;
            default:
                fprintf(stderr, "ERROR: unsupported ioRead length %d\n", len);
                return 1;
        }
    }
    else {
        tmpdata64 = out64.data;
        switch (len) {
            case 8: memcpy(buf, &tmpdata64, 8); break;
            default:
                fprintf(stderr, "ERROR: unsupported ioRead length %d\n", len);
                return 1;
        }
    }

    return 0;
}

static int darwin_iowrite(int pos, unsigned char * buf, int len)
{
    kern_return_t err;
    size_t dataInLen;
    size_t dataOutLen;
    void *in;
    void *out;
    iomem_t in32;
    iomem_t out32;
    iomem64_t in64;
    iomem64_t out64;

    if (len <= 4) {
        in = &in32;
        out = &out32;
        dataInLen = sizeof(in32);
        dataOutLen = sizeof(out32);
        in32.width = len;
        in32.offset = pos;
        memcpy(&in32.data, buf, len);
    }
    else if (len <= 8) {
        in = &in64;
        out = &out64;
        dataInLen = sizeof(in64);
        dataOutLen = sizeof(out64);
        in64.width = len;
        in64.offset = pos;
        memcpy(&in64.data, buf, len);
    }
    else {
        return 1;
    }

    err = MyIOConnectCallStructMethod(connect, kWriteIO, in, dataInLen, out, &dataOutLen);
    if (err != KERN_SUCCESS) {
        return 1;
    }

    return 0;
}


/* Compatibility interface */

unsigned char inb(unsigned short addr)
{
    unsigned char ret = 0;
    darwin_ioread(addr, &ret, 1);
    return ret;
}

unsigned short inw(unsigned short addr)
{
    unsigned short ret = 0;
    darwin_ioread(addr, (unsigned char *)&ret, 2);
    return ret;
}

unsigned int inl(unsigned short addr)
{
    unsigned int ret = 0;
    darwin_ioread(addr, (unsigned char *)&ret, 4);
    return ret;
}

#ifdef __LP64__
unsigned long inq(unsigned short addr)
{
    unsigned long ret = 0;
    darwin_ioread(addr, (unsigned char *)&ret, 8);
    return ret;
}
#endif

void outb(unsigned char val, unsigned short addr)
{
    darwin_iowrite(addr, &val, 1);
}

void outw(unsigned short val, unsigned short addr)
{
    darwin_iowrite(addr, (unsigned char *)&val, 2);
}

void outl(unsigned int val, unsigned short addr)
{
    darwin_iowrite(addr, (unsigned char *)&val, 4);
}

#ifdef __LP64__
void outq(unsigned long val, unsigned short addr)
{
    darwin_iowrite(addr, (unsigned char *)&val, 8);
}
#endif

int iopl(int level __attribute__((unused)))
{
    atexit(darwin_cleanup);
    return darwin_init();
}

void *map_physical(uint64_t phys_addr, size_t len)
{
    kern_return_t err;
#if defined(__LP64__) && (MAC_OS_X_VERSION_SDK >= MAC_OS_X_VERSION_10_5)
    mach_vm_address_t addr;
    mach_vm_size_t size;
#else
    vm_address_t addr;
    vm_size_t size;
#endif
    size_t dataInLen = sizeof(map_t);
    size_t dataOutLen = sizeof(map_t);

    map_t in;
    map_t out;

    in.addr = phys_addr;
    in.size = len;

#ifdef DEBUG
    printf("map_phys: phys %08lx, %08x\n", phys_addr, len);
#endif

    err = MyIOConnectCallStructMethod(connect, kPrepareMap, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS) {
        printf("\nError(kPrepareMap): system 0x%x subsystem 0x%x code 0x%x ",
               err_get_system(err), err_get_sub(err), err_get_code(err));

        printf("physical 0x%16lx[0x%lx]\n", (unsigned long)phys_addr, (unsigned long)len);

        switch (err_get_code(err)) {
            case 0x2c2: printf("Invalid argument.\n"); errno = EINVAL; break;
            case 0x2cd: printf("Device not open.\n"); errno = ENOENT; break;
        }

        return MAP_FAILED;
    }

    err = IOConnectMapMemory(connect, 0, mach_task_self(),
                             &addr, &size, kIOMapAnywhere | kIOMapInhibitCache);

    /* Now this is odd; The above connect seems to be unfinished at the
     * time the function returns. So wait a little bit, or the calling
     * program will just segfault. Bummer. Who knows a better solution?
     */
    usleep(1000);

    if (err != KERN_SUCCESS) {
        printf("\nError(IOConnectMapMemory): system 0x%x subsystem 0x%x code 0x%x ",
               err_get_system(err), err_get_sub(err), err_get_code(err));

        printf("physical 0x%16lx[0x%lx]\n", (unsigned long)phys_addr, (unsigned long)len);

        switch (err_get_code(err)) {
            case 0x2c2: printf("Invalid argument.\n"); errno = EINVAL; break;
            case 0x2cd: printf("Device not open.\n"); errno = ENOENT; break;
        }

        return MAP_FAILED;
    }

#ifdef DEBUG
    printf("map_phys: virt %16lx, %16lx\n", (unsigned  long)addr, (unsigned long)size);
#endif /* DEBUG */

    return (void *)addr;
}

void unmap_physical(void *virt_addr __attribute__((unused)), size_t len __attribute__((unused)))
{
    // Nut'n Honey
}

static int current_logical_cpu = 0;

msr_t rdmsr(int addr)
{
    kern_return_t err;
    size_t dataInLen = sizeof(msrcmd_t);
    size_t dataOutLen = sizeof(msrcmd_t);
    msrcmd_t in, out;
    msr_t ret = { INVALID_MSR_HI, INVALID_MSR_LO };

    in.core = current_logical_cpu;
    in.index = addr;

    err = MyIOConnectCallStructMethod(connect, kReadMSR, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS) {
        return ret;
    }

    ret.io64 = out.val.io64;

    return ret;
}

int rdcpuid(uint32_t eax, uint32_t ecx, uint32_t cpudata[4])
{
    kern_return_t err;
    size_t dataInLen = sizeof(cpuid_t);
    size_t dataOutLen = sizeof(cpuid_t);
    cpuid_t in, out;

    in.core = current_logical_cpu;
    in.eax = eax;
    in.ecx = ecx;

    err = MyIOConnectCallStructMethod(connect, kReadCpuId, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return -1;

    memcpy(cpudata, out.cpudata, sizeof(uint32_t) * 4);
    return 0;
}

int readmem32(uint64_t addr, uint32_t* data)
{
    kern_return_t err;
    size_t dataInLen = sizeof(readmem_t);
    size_t dataOutLen = sizeof(readmem_t);
    readmem_t in, out;

    in.core = current_logical_cpu;
    in.addr = addr;

    err = MyIOConnectCallStructMethod(connect, kReadMem, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return -1;

    *data = out.data;
    return 0;
}

int wrmsr(int addr, msr_t msr)
{
    kern_return_t err;
    size_t dataInLen = sizeof(msrcmd_t);
    size_t dataOutLen = sizeof(msrcmd_t);
    msrcmd_t in;
    msrcmd_t out;

    in.core = current_logical_cpu;
    in.index = addr;
    in.val.io64 = msr.io64;

    err = MyIOConnectCallStructMethod(connect, kWriteMSR, &in, dataInLen, &out, &dataOutLen);
    if (err != KERN_SUCCESS)
        return 1;

    return 0;
}

int logical_cpu_select(int cpu)
{
    current_logical_cpu = cpu;
    return current_logical_cpu;
}
