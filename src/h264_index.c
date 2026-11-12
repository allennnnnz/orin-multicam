/*
 * h264_index.c - Annex-B access-unit indexer.
 *
 * Access-unit boundary rule (simplified H.264 7.4.1.2.3):
 *   a new AU starts at the first non-VCL NAL (AUD/SPS/PPS/SEI, types 6..9)
 *   or at the first VCL NAL with first_mb_in_slice == 0 that follows a VCL NAL
 *   of the current AU.
 */
#include "h264_index.h"
#include "common.h"

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/* Return the offset of the next start code (00 00 01 or 00 00 00 01) at or
 * after pos; *sc_len receives 3 or 4. Returns size if none. */
static size_t next_start_code(const uint8_t *d, size_t size, size_t pos, int *sc_len)
{
    for (size_t i = pos; i + 3 <= size; i++) {
        if (d[i] == 0 && d[i + 1] == 0) {
            if (d[i + 2] == 1) { *sc_len = 3; return i; }
            if (i + 4 <= size && d[i + 2] == 0 && d[i + 3] == 1) { *sc_len = 4; return i; }
        }
    }
    *sc_len = 0;
    return size;
}

static int push(h264_index_t *ix, uint32_t *cap, uint64_t off, uint32_t len, int has_idr)
{
    if (ix->n_au == *cap) {
        *cap = *cap ? *cap * 2 : 1024;
        uint64_t *o = realloc(ix->au_off, *cap * sizeof(*o));
        uint32_t *l = realloc(ix->au_len, *cap * sizeof(*l));
        uint32_t *r = realloc(ix->idr_au, *cap * sizeof(*r));
        if (!o || !l || !r) return -1;
        ix->au_off = o; ix->au_len = l; ix->idr_au = r;
    }
    if (has_idr) ix->idr_au[ix->n_idr++] = ix->n_au;
    ix->au_off[ix->n_au] = off;
    ix->au_len[ix->n_au] = len;
    if (len > ix->max_au_len) ix->max_au_len = len;
    ix->n_au++;
    return 0;
}

int h264_index_open(h264_index_t *ix, const char *path)
{
    memset(ix, 0, sizeof(*ix));
    int fd = open(path, O_RDONLY);
    if (fd < 0) { ERRNO("open %s", path); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { ERRNO("fstat"); close(fd); return -1; }
    ix->size = (size_t)st.st_size;
    void *p = mmap(NULL, ix->size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) { ERRNO("mmap"); return -1; }
    ix->data = p;

    const uint8_t *d = ix->data;
    uint32_t cap = 0;
    int sc;
    size_t pos = next_start_code(d, ix->size, 0, &sc);
    size_t au_start = pos;
    int au_has_vcl = 0, au_has_idr = 0;

    while (pos < ix->size) {
        size_t hdr = pos + (size_t)sc;
        if (hdr >= ix->size) break;
        int type = d[hdr] & 0x1f;
        int is_vcl = (type == 1 || type == 5);
        /* first_mb_in_slice is ue(v); value 0 is coded as a single '1' bit */
        int first_mb0 = is_vcl && hdr + 1 < ix->size && (d[hdr + 1] & 0x80);
        int starts_new = au_has_vcl &&
                         ((type >= 6 && type <= 9) || (is_vcl && first_mb0));
        if (starts_new) {
            if (push(ix, &cap, au_start, (uint32_t)(pos - au_start), au_has_idr) < 0) goto oom;
            au_start = pos;
            au_has_vcl = au_has_idr = 0;
        }
        if (is_vcl) au_has_vcl = 1;
        if (type == 5) au_has_idr = 1;
        pos = next_start_code(d, ix->size, hdr, &sc);
    }
    if (au_has_vcl &&
        push(ix, &cap, au_start, (uint32_t)(ix->size - au_start), au_has_idr) < 0)
        goto oom;

    if (ix->n_au == 0 || ix->n_idr == 0) {
        ERR("%s: no decodable access units found", path);
        h264_index_close(ix);
        return -1;
    }
    return 0;
oom:
    ERR("out of memory while indexing");
    h264_index_close(ix);
    return -1;
}

void h264_index_close(h264_index_t *ix)
{
    if (ix->data) munmap((void *)ix->data, ix->size);
    free(ix->au_off);
    free(ix->au_len);
    free(ix->idr_au);
    memset(ix, 0, sizeof(*ix));
}
