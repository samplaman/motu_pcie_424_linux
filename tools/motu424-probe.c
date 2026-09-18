// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * motu424-probe - userspace helper for reverse engineering the MOTU
 * PCI-324/424 register map.
 *
 * It scans /sys/bus/pci for Mark of the Unicorn devices (vendor 0x137A),
 * prints their identity, enumerates every BAR and classifies it against the
 * windowed hardware model recovered from the vendor driver (see
 * docs/register-map.md):
 *
 *   - an ~8 MB MMIO BAR  -> window A  (card addr & 0x7fffff)
 *   - a  ~4 MB MMIO BAR  -> window B  (card addr & 0x3fffff, the audio aperture)
 *   - a small I/O-port BAR -> the bridge/GPIO control (IRQ status, strobes)
 *
 * The registers of interest are NOT at BAR offset 0: they live at window-B card
 * addresses (e.g. bank ctrl/status at 0xC0024 / 0x100024). This tool therefore
 * dumps both the head of each BAR and those known card offsets, and takes an
 * optional targeted window-B dump so idle-vs-streaming diffs can locate the
 * runtime audio base / ack / aperture addresses the driver needs.
 *
 * The in-tree kernel driver must NOT be bound to the device at the same time.
 *
 *   sudo ./motu424-probe                     # scan + classify + dump all cards
 *   sudo ./motu424-probe 0000:01:00.0        # one card
 *   sudo ./motu424-probe 0000:01:00.0 0xc0000 0x400   # window-B dump @off,len
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#if (defined(__i386__) || defined(__x86_64__)) && defined(__linux__)
#include <sys/io.h>
#define HAVE_SYS_IO 1
#endif

#define MOTU_VENDOR 0x137A	/* Mark of the Unicorn (confirmed, MOTUAW.inf) */
#define SYS_PCI     "/sys/bus/pci/devices"
#define PCI_NUM_BARS 6
#define HEAD_BYTES  0x100	/* how much of each BAR head to dump */

/* IORESOURCE flag bits as exposed in the sysfs `resource` file. */
#define RES_IO      0x00000100ul
#define RES_MEM     0x00000200ul

/* Window masks + the confirmed bank ctrl/status card offsets. */
#define WINA_TAG    0x01800000ul
#define WINA_MASK   0x007ffffful
#define WINB_MASK   0x003ffffful
#define BANK0_CTRL  0x000c0024ul
#define BANK1_CTRL  0x00100024ul

struct bar {
	uint64_t start, end, flags;
	uint64_t size;
};

static unsigned read_hex_file(const char *dir, const char *file)
{
	char path[600];
	unsigned val = 0;
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	f = fopen(path, "r");
	if (!f)
		return 0;
	if (fscanf(f, "%x", &val) != 1)
		val = 0;
	fclose(f);
	return val;
}

/* Parse the sysfs `resource` file: one "start end flags" line per BAR. */
static int read_bars(const char *dir, struct bar bars[PCI_NUM_BARS])
{
	char path[600];
	FILE *f;
	int i, n = 0;

	snprintf(path, sizeof(path), "%s/resource", dir);
	f = fopen(path, "r");
	if (!f)
		return 0;
	for (i = 0; i < PCI_NUM_BARS; i++) {
		if (fscanf(f, "%" SCNx64 " %" SCNx64 " %" SCNx64,
			   &bars[i].start, &bars[i].end, &bars[i].flags) != 3)
			break;
		bars[i].size = bars[i].start ? bars[i].end - bars[i].start + 1 : 0;
		if (bars[i].size)
			n++;
	}
	fclose(f);
	return n;
}

static const char *classify(const struct bar *b)
{
	if (b->flags & RES_IO)
		return "I/O-port bridge (port BAR)";
	if (b->flags & RES_MEM) {
		if (b->size >= 0x800000)
			return "window A (>=8 MB MMIO)";
		if (b->size >= 0x200000)
			return "window B (~4 MB MMIO, audio aperture)";
		return "MMIO (small)";
	}
	return "unknown";
}

/* mmap resourceN and return the base, or NULL. Sets *len to the mapped span. */
static volatile uint32_t *map_bar(const char *dir, int idx, uint64_t barsize,
				  uint64_t off, size_t want, size_t *len)
{
	char path[600];
	volatile uint32_t *p;
	int fd;

	if (off + want > barsize)
		want = off < barsize ? (size_t)(barsize - off) : 0;
	if (!want)
		return NULL;

	snprintf(path, sizeof(path), "%s/resource%d", dir, idx);
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		perror("open resourceN (need root, and driver unbound)");
		return NULL;
	}
	/* Page-align the mmap offset; sysfs resource mmap requires it. */
	p = mmap(NULL, want, PROT_READ, MAP_SHARED, fd, off);
	close(fd);
	if (p == MAP_FAILED) {
		perror("mmap");
		return NULL;
	}
	*len = want;
	return p;
}

static void hexdump(volatile uint32_t *base, uint64_t card_off, size_t len)
{
	for (size_t o = 0; o + 4 <= len; o += 16) {
		printf("    %08" PRIx64 ":", card_off + o);
		for (size_t w = 0; w < 16 && o + w + 4 <= len; w += 4)
			printf(" %08x", base[(o + w) / 4]);
		printf("\n");
	}
}

/* Dump the head of a BAR plus, for windows A/B, the known bank ctrl/status regs. */
static void dump_bar(const char *dir, int idx, const struct bar *b)
{
	volatile uint32_t *p;
	size_t len;

	if (b->flags & RES_IO) {
#ifdef HAVE_SYS_IO
		if (iopl(3) == 0) {
			uint32_t p0 = inl(b->start + 0);
			uint32_t p4 = inl(b->start + 4);
			uint32_t p8 = inl(b->start + 8);
			uint32_t pc = inl(b->start + 12);
			printf("    ports @ 0x%04" PRIx64 ":\n", b->start);
			printf("      +0x0 (status/ctrl) = 0x%08x  [irq_pending=%u, enable=%u]\n",
			       p0, (p0 >> 1) & 1, (p0 >> 2) & 1);
			printf("      +0x4 (strobe)      = 0x%08x\n", p4);
			printf("      +0x8 (page/init)   = 0x%08x\n", p8);
			printf("      +0xc (bridge/id)   = 0x%08x\n", pc);
		} else {
			perror("iopl (need root for port I/O)");
		}
#else
		printf("    (port I/O access only supported on x86/x86_64)\n");
#endif
		return;
	}

	p = map_bar(dir, idx, b->size, 0, HEAD_BYTES, &len);
	if (p) {
		printf("    head:\n");
		hexdump(p, 0, len);
		munmap((void *)p, len);
	}

	if ((b->flags & RES_MEM) && b->size >= 0x200000 && b->size < 0x800000) {
		uint64_t regs[] = { BANK0_CTRL & WINB_MASK, BANK1_CTRL & WINB_MASK };

		for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
			p = map_bar(dir, idx, b->size, regs[i] & ~0xfffUL,
				    0x1000, &len);
			if (!p)
				continue;
			printf("    bank ctrl @ card 0x%08" PRIx64 " (win-B): %08x\n",
			       regs[i], p[(regs[i] & 0xfff) / 4]);
			munmap((void *)p, len);
		}
	} else if ((b->flags & RES_MEM) && b->size >= 0x800000) {
		uint64_t regs[] = { BANK0_CTRL & WINA_MASK, BANK1_CTRL & WINA_MASK };

		for (size_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
			p = map_bar(dir, idx, b->size, regs[i] & ~0xfffUL,
				    0x1000, &len);
			if (!p)
				continue;
			printf("    bank ctrl @ card 0x%08" PRIx64 " (win-A): %08x\n",
			       WINA_TAG | regs[i], p[(regs[i] & 0xfff) / 4]);
			munmap((void *)p, len);
		}
	}
}

/* Optional user-requested targeted window A or B dump. */
static void dump_target(const char *dir, const struct bar bars[PCI_NUM_BARS],
			uint64_t card_off, size_t len)
{
	int idx = -1;
	uint64_t off;
	const char *wname;

	if ((card_off & 0xff800000ul) == WINA_TAG) {
		wname = "window-A";
		for (int i = 0; i < PCI_NUM_BARS; i++)
			if ((bars[i].flags & RES_MEM) && bars[i].size >= 0x800000)
				idx = i;
		off = card_off & WINA_MASK;
	} else {
		wname = "window-B";
		for (int i = 0; i < PCI_NUM_BARS; i++)
			if ((bars[i].flags & RES_MEM) && bars[i].size >= 0x200000 &&
			    bars[i].size < 0x800000)
				idx = i;
		off = card_off & WINB_MASK;
	}

	if (idx < 0) {
		fprintf(stderr, "  no %s BAR to target\n", wname);
		return;
	}
	uint64_t page = off & ~0xfffUL;
	volatile uint32_t *p;
	size_t got;

	p = map_bar(dir, idx, bars[idx].size, page, (len + 0xfff) & ~0xfffUL,
		    &got);
	if (!p)
		return;
	/*
	 * map_bar() clamps the mapped span to the BAR's actual size (got may
	 * be less than requested near the end of a BAR); clamp the dump
	 * length the same way, or hexdump() reads past the mmap'd region.
	 */
	if (len > got - (off - page))
		len = got - (off - page);
	printf("  %s target @ card 0x%08" PRIx64 ", %zu bytes (BAR%d, off 0x%" PRIx64 "):\n",
	       wname, card_off, len, idx, off);
	hexdump(p + (off - page) / 4, card_off, len);
	munmap((void *)p, got);
}

static void handle(const char *bdf, uint64_t tgt_off, size_t tgt_len)
{
	char dir[512];
	struct bar bars[PCI_NUM_BARS] = { 0 };
	unsigned device;
	int i;

	snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, bdf);
	device = read_hex_file(dir, "device");
	printf("MOTU PCI device %s: device=%04x (%s)\n", bdf, device,
	       device == 0x0003 ? "PCI-324" :
	       device == 0x0004 ? "PCI-424" :
	       device == 0x0005 ? "PCIe-424 (HD Express)" : "unknown/other");

	read_bars(dir, bars);
	for (i = 0; i < PCI_NUM_BARS; i++) {
		if (!bars[i].size)
			continue;
		printf("  BAR%d: 0x%08" PRIx64 "..0x%08" PRIx64
		       " (%" PRIu64 " KB) flags=0x%" PRIx64 " -> %s\n",
		       i, bars[i].start, bars[i].end, bars[i].size >> 10,
		       bars[i].flags, classify(&bars[i]));
		dump_bar(dir, i, &bars[i]);
	}

	if (tgt_len)
		dump_target(dir, bars, tgt_off, tgt_len);
}

int main(int argc, char **argv)
{
	DIR *d;
	struct dirent *e;
	const char *want = argc > 1 ? argv[1] : NULL;
	uint64_t tgt_off = argc > 2 ? strtoull(argv[2], NULL, 0) : 0;
	size_t tgt_len = argc > 3 ? strtoull(argv[3], NULL, 0) : 0;
	int found = 0;

	d = opendir(SYS_PCI);
	if (!d) {
		perror("opendir " SYS_PCI);
		return 1;
	}
	while ((e = readdir(d))) {
		char dir[512];

		if (e->d_name[0] == '.')
			continue;
		if (want && strcmp(e->d_name, want))
			continue;
		snprintf(dir, sizeof(dir), "%s/%s", SYS_PCI, e->d_name);
		if (!want && read_hex_file(dir, "vendor") != MOTU_VENDOR)
			continue;
		handle(e->d_name, tgt_off, tgt_len);
		found++;
	}
	closedir(d);

	if (!found) {
		fprintf(stderr,
			"No MOTU (vendor %04x) PCI device found.\n"
			"If the card is present, check `lspci -nn | grep %04x`.\n",
			MOTU_VENDOR, MOTU_VENDOR);
		return 2;
	}
	return 0;
}
