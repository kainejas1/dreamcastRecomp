#include "gdrom.h"
#include "memory.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HD_AREA_LBA 45000u
#define MAX_TRACKS  99

typedef struct {
    int number;
    int audio;
    FILE *f;
    uint32_t file_lba;   // LBA of the first sector in the file
    uint32_t start_lba;  // LBA of INDEX 01 (where the track "starts")
    uint32_t nsectors;   // sectors in the file
    uint32_t sector_size;
    uint32_t data_offset; // 16 for MODE1/2352 (sync + header), 0 otherwise
} track;

static track tracks[MAX_TRACKS];
static int ntracks;

int gd_is_open(void) { return ntracks > 0; }

static uint32_t msf_to_frames(const char *s)
{
    unsigned m = 0, sec = 0, fr = 0;
    sscanf(s, "%u:%u:%u", &m, &sec, &fr);
    return (m * 60 + sec) * 75 + fr;
}

// .cue parser, just enough for Redump GD-ROM dumps:
//   REM HIGH-DENSITY AREA / FILE "x.bin" BINARY / TRACK nn MODE1/2352|AUDIO / INDEX 01 mm:ss:ff
int gd_open(const char *cue_path, char *err, int err_size)
{
    FILE *cue = fopen(cue_path, "r");
    if (!cue) { snprintf(err, err_size, "cannot open '%s'", cue_path); return 0; }

    // track files are next to the .cue
    char dir[512] = "";
    const char *slash = strrchr(cue_path, '/'), *bslash = strrchr(cue_path, '\\');
    if (bslash > slash) slash = bslash;
    if (slash) snprintf(dir, sizeof dir, "%.*s", (int)(slash - cue_path + 1), cue_path);

    char line[512], file_name[512] = "";
    uint32_t next_lba = 0;   // where the next file starts
    FILE *cur = NULL;
    uint32_t cur_lba = 0, cur_sectors = 0;

    while (fgets(line, sizeof line, cue)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;

        if (!strncmp(p, "REM HIGH-DENSITY AREA", 21)) {
            next_lba = HD_AREA_LBA;
        } else if (!strncmp(p, "FILE ", 5)) {
            char *q1 = strchr(p, '"'), *q2 = q1 ? strchr(q1 + 1, '"') : NULL;
            if (!q1 || !q2) continue;
            if (snprintf(file_name, sizeof file_name, "%s%.*s", dir, (int)(q2 - q1 - 1), q1 + 1) >= (int)sizeof file_name)
                continue;   // path too long
            cur = fopen(file_name, "rb");
            if (!cur) { snprintf(err, err_size, "cannot open track file '%s'", file_name); fclose(cue); return 0; }
            fseek(cur, 0, SEEK_END);
            long size = ftell(cur);
            cur_sectors = (uint32_t)(size / 2352);
            cur_lba = next_lba;
            next_lba += cur_sectors;
        } else if (!strncmp(p, "TRACK ", 6) && cur && ntracks < MAX_TRACKS) {
            track *t = &tracks[ntracks++];
            char mode[32] = "";
            sscanf(p + 6, "%d %31s", &t->number, mode);
            t->audio = !strncmp(mode, "AUDIO", 5);
            t->f = cur;
            t->file_lba = t->start_lba = cur_lba;
            t->nsectors = cur_sectors;
            t->sector_size = 2352;
            t->data_offset = !strcmp(mode, "MODE1/2352") ? 16 : 0;
        } else if (!strncmp(p, "INDEX 01 ", 9) && ntracks) {
            tracks[ntracks - 1].start_lba = tracks[ntracks - 1].file_lba + msf_to_frames(p + 9);
        }
    }
    fclose(cue);
    if (!ntracks) { snprintf(err, err_size, "no tracks in '%s'", cue_path); return 0; }
    return 1;
}

static const track *find_track(uint32_t lba)
{
    for (int i = 0; i < ntracks; i++)
        if (lba >= tracks[i].file_lba && lba < tracks[i].file_lba + tracks[i].nsectors)
            return &tracks[i];
    return NULL;
}

uint32_t gd_read(uint32_t fad, uint32_t count, uint32_t dest)
{
    uint8_t buf[2048];
    uint32_t done;

    for (done = 0; done < count; done++) {
        uint32_t lba = fad + done - 150;
        const track *t = find_track(lba);
        if (!t || t->audio) {
            fprintf(stderr, "[gd] read of FAD %u: no data track there\n", fad + done);
            break;
        }
        long off = (long)(lba - t->file_lba) * t->sector_size + t->data_offset;
        if (fseek(t->f, off, SEEK_SET) != 0 || fread(buf, 1, sizeof buf, t->f) != sizeof buf) {
            fprintf(stderr, "[gd] read error at FAD %u\n", fad + done);
            break;
        }
        uint32_t a = dest + done * 2048;
        uint8_t *p = mem_ptr(a, sizeof buf);
        if (p) memcpy(p, buf, sizeof buf);
        else for (int i = 0; i < 2048; i++) mem_w8(a + i, buf[i]);
    }
    return done;
}

void gd_toc(int area, uint32_t toc[102])
{
    int first = 0, last = 0;
    uint32_t leadout = 0, first_ctrl = 0, last_ctrl = 0;

    for (int i = 0; i < 99; i++) toc[i] = 0xffffffffu;
    for (int i = 0; i < ntracks; i++) {
        const track *t = &tracks[i];
        int hd = t->file_lba >= HD_AREA_LBA;
        if (hd != (area == 1)) continue;
        uint32_t ctrl = t->audio ? 0 : 4;
        toc[t->number - 1] = ctrl << 28 | 1u << 24 | (t->start_lba + 150);
        if (!first) { first = t->number; first_ctrl = ctrl; }
        last = t->number;
        last_ctrl = ctrl;
        leadout = t->file_lba + t->nsectors + 150;
    }
    toc[99]  = first_ctrl << 28 | 1u << 24 | (uint32_t)first << 16;
    toc[100] = last_ctrl << 28 | 1u << 24 | (uint32_t)last << 16;
    toc[101] = last_ctrl << 28 | 1u << 24 | leadout;
}
