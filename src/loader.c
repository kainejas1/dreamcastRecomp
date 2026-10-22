#include "loader.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EM_SH          42
#define PT_LOAD        1
#define PF_X           1
#define SHT_PROGBITS   1
#define SHT_SYMTAB     2
#define SHF_ALLOC      2
#define SHF_EXECINSTR  4

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return 0;
}

static uint8_t *read_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len < 0) { fclose(f); return NULL; }
    uint8_t *buf = malloc(len ? (size_t)len : 1);
    if (buf && fread(buf, 1, (size_t)len, f) != (size_t)len) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)len;
    return buf;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

int dc_ram_offset(uint32_t addr, uint32_t *off)
{
    uint32_t phys = addr & 0x1FFFFFFFu;
    if (phys < DC_RAM_PHYS || phys >= DC_RAM_PHYS + DC_RAM_SIZE) return 0;
    *off = phys - DC_RAM_PHYS;
    return 1;
}

int dc_read16(const dc_image *img, uint32_t addr, uint16_t *out)
{
    uint32_t off;
    if (!dc_ram_offset(addr, &off) || off + 2 > DC_RAM_SIZE) return 0;
    *out = le16(img->ram + off);
    return 1;
}

int dc_read32(const dc_image *img, uint32_t addr, uint32_t *out)
{
    uint32_t off;
    if (!dc_ram_offset(addr, &off) || off + 4 > DC_RAM_SIZE) return 0;
    *out = le32(img->ram + off);
    return 1;
}

static int copy_to_ram(dc_image *img, uint32_t vaddr, const uint8_t *src, uint32_t len)
{
    uint32_t off;
    if (!dc_ram_offset(vaddr, &off) || (uint64_t)off + len > DC_RAM_SIZE) return 0;
    memcpy(img->ram + off, src, len);
    return 1;
}

static void add_region(dc_image *img, uint32_t start, uint32_t end, const char *name)
{
    img->regions = realloc(img->regions, (size_t)(img->nregions + 1) * sizeof *img->regions);
    dc_region *r = &img->regions[img->nregions++];
    r->start = start & ~1u;
    r->end = end;
    snprintf(r->name, sizeof r->name, "%s", name);
}

static int sym_cmp(const void *a, const void *b)
{
    const dc_symbol *x = a, *y = b;
    return (x->addr > y->addr) - (x->addr < y->addr);
}

static int image_init(dc_image *img)
{
    memset(img, 0, sizeof *img);
    img->ram = calloc(1, DC_RAM_SIZE);
    return img->ram != NULL;
}

int dc_load_bin(dc_image *img, const char *path, uint32_t base, char *err, size_t errlen)
{
    size_t size;
    uint8_t *buf = read_file(path, &size);
    if (!buf) return fail(err, errlen, "cannot read %s", path);
    if (!image_init(img)) { free(buf); return fail(err, errlen, "out of memory"); }
    if (!copy_to_ram(img, base, buf, (uint32_t)size)) {
        free(buf);
        dc_image_free(img);
        return fail(err, errlen, "binary (%zu bytes) does not fit in RAM at 0x%08x", size, base);
    }
    free(buf);
    img->entry = base;
    add_region(img, base, base + (uint32_t)size, "bin");
    return 1;
}

int dc_load_elf(dc_image *img, const char *path, char *err, size_t errlen)
{
    size_t size;
    uint8_t *f = read_file(path, &size);
    if (!f) return fail(err, errlen, "cannot read %s", path);

    if (size < 52 || memcmp(f, "\x7f" "ELF", 4) != 0) { free(f); return fail(err, errlen, "not an ELF file"); }
    if (f[4] != 1 || f[5] != 1) { free(f); return fail(err, errlen, "expected 32-bit little-endian ELF"); }
    uint16_t machine = le16(f + 18);
    if (machine != EM_SH) { free(f); return fail(err, errlen, "e_machine is %u, expected SuperH (42)", machine); }

    if (!image_init(img)) { free(f); return fail(err, errlen, "out of memory"); }

    img->entry = le32(f + 24);
    uint32_t phoff = le32(f + 28), shoff = le32(f + 32);
    uint16_t phentsize = le16(f + 42), phnum = le16(f + 44);
    uint16_t shentsize = le16(f + 46), shnum = le16(f + 48), shstrndx = le16(f + 50);

    /* Program headers: copy loadable segments into RAM. */
    for (uint16_t i = 0; i < phnum; i++) {
        size_t o = (size_t)phoff + (size_t)i * phentsize;
        if (o + 32 > size) break;
        const uint8_t *ph = f + o;
        if (le32(ph) != PT_LOAD) continue;
        uint32_t offset = le32(ph + 4), vaddr = le32(ph + 8), filesz = le32(ph + 16);
        if ((uint64_t)offset + filesz > size || !copy_to_ram(img, vaddr, f + offset, filesz)) {
            free(f);
            dc_image_free(img);
            return fail(err, errlen, "PT_LOAD segment %u at 0x%08x is out of range", i, vaddr);
        }
    }

    /* Section headers: executable sections and the symbol table. */
    const uint8_t *shstr = NULL;
    size_t shstr_size = 0;
    if (shoff && shstrndx < shnum && (size_t)shoff + (size_t)(shstrndx + 1) * shentsize <= size) {
        const uint8_t *sh = f + shoff + (size_t)shstrndx * shentsize;
        uint32_t o = le32(sh + 16), s = le32(sh + 20);
        if ((uint64_t)o + s <= size) { shstr = f + o; shstr_size = s; }
    }

    for (uint16_t i = 0; shoff && i < shnum; i++) {
        size_t o = (size_t)shoff + (size_t)i * shentsize;
        if (o + 40 > size) break;
        const uint8_t *sh = f + o;
        uint32_t name = le32(sh), type = le32(sh + 4), flags = le32(sh + 8);
        uint32_t addr = le32(sh + 12), offset = le32(sh + 16), ssize = le32(sh + 20), link = le32(sh + 24);

        if (type == SHT_PROGBITS && (flags & (SHF_ALLOC | SHF_EXECINSTR)) == (SHF_ALLOC | SHF_EXECINSTR) && ssize) {
            const char *nm = (shstr && name < shstr_size) ? (const char *)shstr + name : "?";
            add_region(img, addr, addr + ssize, nm);
        }

        if (type == SHT_SYMTAB && link < shnum && (uint64_t)offset + ssize <= size) {
            const uint8_t *strsh = f + shoff + (size_t)link * shentsize;
            uint32_t stro = le32(strsh + 16), strsz = le32(strsh + 20);
            if ((uint64_t)stro + strsz > size) continue;
            for (uint32_t s = 0; s + 16 <= ssize; s += 16) {
                const uint8_t *sym = f + offset + s;
                uint32_t sname = le32(sym), value = le32(sym + 4), sz = le32(sym + 8);
                uint8_t stype = sym[12] & 0xF;
                uint16_t shndx = le16(sym + 14);
                uint32_t dummy;
                if ((stype != 0 && stype != 2) || shndx == 0 || sname == 0 || sname >= strsz) continue;
                if (!dc_ram_offset(value, &dummy)) continue;
                const char *str = (const char *)f + stro + sname;
                img->syms = realloc(img->syms, (size_t)(img->nsyms + 1) * sizeof *img->syms);
                img->syms[img->nsyms].addr = value;
                img->syms[img->nsyms].size = sz;
                img->syms[img->nsyms].name = strdup(str);
                img->nsyms++;
            }
        }
    }

    /* No section headers (stripped): fall back to executable segments. */
    if (img->nregions == 0) {
        for (uint16_t i = 0; i < phnum; i++) {
            size_t o = (size_t)phoff + (size_t)i * phentsize;
            if (o + 32 > size) break;
            const uint8_t *ph = f + o;
            if (le32(ph) == PT_LOAD && (le32(ph + 24) & PF_X))
                add_region(img, le32(ph + 8), le32(ph + 8) + le32(ph + 16), "segment");
        }
    }

    if (img->nsyms) qsort(img->syms, (size_t)img->nsyms, sizeof *img->syms, sym_cmp);
    free(f);
    return 1;
}

const char *dc_symbol_at(const dc_image *img, uint32_t addr)
{
    int lo = 0, hi = img->nsyms - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (img->syms[mid].addr < addr) lo = mid + 1;
        else if (img->syms[mid].addr > addr) hi = mid - 1;
        else {
            while (mid > 0 && img->syms[mid - 1].addr == addr) mid--;
            return img->syms[mid].name;
        }
    }
    return NULL;
}

int dc_image_copy(dc_image *img, uint32_t dest, uint32_t src, uint32_t len, char *err, size_t errlen)
{
    uint32_t so, d;
    if (!len || (dest & 1) || !dc_ram_offset(src, &so) || !dc_ram_offset(dest, &d) ||
        (uint64_t)so + len > DC_RAM_SIZE || (uint64_t)d + len > DC_RAM_SIZE)
        return fail(err, errlen, "bad copy %08x=%08x,%x", dest, src, len);
    memmove(img->ram + d, img->ram + so, len);
    add_region(img, dest, dest + len, "copy");
    img->roots = realloc(img->roots, (size_t)(img->nroots + 1) * sizeof *img->roots);
    img->roots[img->nroots++] = dest;
    return 1;
}

void dc_image_free(dc_image *img)
{
    for (int i = 0; i < img->nsyms; i++) free(img->syms[i].name);
    free(img->syms);
    free(img->roots);
    free(img->regions);
    free(img->ram);
    memset(img, 0, sizeof *img);
}
