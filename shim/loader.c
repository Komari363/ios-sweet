/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

static uint32_t ld32(const volatile uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t ld64(const volatile uint8_t *p)
{
	uint64_t v = 0;
	for (int i = 7; i >= 0; i--)
		v = (v << 8) | p[i];
	return v;
}

static void copy_bytes(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint8_t *d = (volatile uint8_t *)dst;
	volatile const uint8_t *s = (volatile const uint8_t *)src;
	for (uint64_t i = 0; i < n; i++)
		d[i] = s[i];
}

/* Source may be unaligned (ABL can prepend bootconfig to the initrd). */
static void copy_seg(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint8_t *s = (volatile const uint8_t *)src;
	uint64_t i = 0;
	for (; i + 8 <= n; i += 8) {
		uint64_t v = (uint64_t)s[i] | ((uint64_t)s[i + 1] << 8) |
			     ((uint64_t)s[i + 2] << 16) | ((uint64_t)s[i + 3] << 24) |
			     ((uint64_t)s[i + 4] << 32) | ((uint64_t)s[i + 5] << 40) |
			     ((uint64_t)s[i + 6] << 48) | ((uint64_t)s[i + 7] << 56);
		d[i / 8] = v;
	}
	for (; i < n; i++)
		((volatile uint8_t *)dst)[i] = s[i];
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;

	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
		return "no initrd in /chosen";
	con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
	if (rd_end <= rd_start)
		return "bad initrd range";

	/*
	 * ABL may prepend a bootconfig (observed 1,155 B on first-light) before
	 * the ramdisk.  Locate the payload magic instead of assuming rd_start.
	 */
	uint64_t rd_len = rd_end - rd_start;
	uint64_t off = 0;
	for (; off + 8 <= rd_len && off < 4096; off++) {
		if (magic_ok((const char *)(uintptr_t)(rd_start + off)))
			break;
	}
	if (off + 8 > rd_len || off >= 4096)
		return "no SWEETXNU payload";
	if (off)
		con_hex("PRE", off);

	const volatile uint8_t *p = (const volatile uint8_t *)(uintptr_t)(rd_start + off);
	uint32_t version = ld32(p + 8);
	uint32_t nsegs = ld32(p + 12);
	if (version != 1 || nsegs > 16)
		return "bad payload header";
	if (off + 0x2a8 + (uint64_t)nsegs * 32 > rd_len)
		return "bad payload header";

	uint64_t virt_base = ld64(p + 16), entry_va = ld64(p + 24);
	uint64_t adt_off = ld64(p + 32), adt_size = ld64(p + 40);
	uint64_t kernel_end = ld64(p + 48), phys_base = ld64(p + 56);
	uint64_t mem_size = ld64(p + 64);
	uint64_t pa_of_va_base = phys_base - virt_base;

	con_hex2("PHYS", "base=", phys_base, "size=", mem_size);
	con_hex("VIRT", virt_base);
	con_hex("ENTRY", entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > phys_base && rd_start + off < phys_base + mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < nsegs; i++) {
		const volatile uint8_t *s = p + 0x2a8 + (uint64_t)i * 32;
		uint64_t vmaddr = ld64(s), vmsize = ld64(s + 8);
		uint64_t file_off = ld64(s + 16), file_size = ld64(s + 24);
		uintptr_t dst = vmaddr + pa_of_va_base;
		copy_seg(dst, rd_start + off + file_off, file_size);
		if (vmsize > file_size)
			zero64(dst + ((file_size + 7) & ~7UL), vmsize - ((file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", nsegs, "end=", kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = kernel_end + pa_of_va_base;
	copy_bytes(adt_pa, rd_start + off + adt_off, adt_size);
	uintptr_t ba_pa = (adt_pa + adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = FB_BASE;
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)adt_size;
	copy_bytes((uintptr_t)ba->CommandLine, rd_start + off + 72, BOOT_LINE_LENGTH);
	ba->memSizeActual = mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
