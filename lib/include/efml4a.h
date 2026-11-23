/* ForTMR2 抽象接口
 *
 * 只保留与资源来源无关的接口：目录 / 内存。
 * VFS 后端不在公开仓；需要时由私有组件通过进程边界提供。
 */
#ifndef EFML4A_H
#define EFML4A_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct efml4a_source efml4a_source;

struct efml4a_source {
    const char *name;
    void       *ctx;

    int   (*open)  (efml4a_source *s);
    void  (*close) (efml4a_source *s);
    long  (*count) (efml4a_source *s);
    int   (*stat)  (efml4a_source *s, long idx,
                    char *name, size_t namesz,
                    long *off, long *len);
    long  (*find)  (efml4a_source *s, const char *name);
    long  (*read)  (efml4a_source *s, long off, long len, uint8_t *out);
    int   (*can_write)(efml4a_source *s);
    long  (*write) (efml4a_source *s, long off, long len, const uint8_t *in);
};

static inline long efml4a_read_all(efml4a_source *s, long off, long len, uint8_t **out)
{
    if (!s || !s->read || len < 0) return -1;
    uint8_t *buf = (uint8_t *)malloc((size_t)len + 1);
    if (!buf) return -1;
    long n = s->read(s, off, len, buf);
    if (n < 0) { free(buf); return -1; }
    buf[n] = 0;
    *out = buf;
    return n;
}

efml4a_source *efml4a_source_dir_new(const char *dirpath);
efml4a_source *efml4a_source_mem_new(uint8_t *data, long len, int take_ownership);
void efml4a_source_free(efml4a_source *s);

#ifdef __cplusplus
}
#endif
#endif
