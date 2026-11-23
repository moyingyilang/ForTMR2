#include "efml4a.h"
#include <stdio.h>
#include <string.h>

static void list_source(efml4a_source *s, int maxshow) {
    if (!s) { printf("  (null source)\n"); return; }
    if (s->open(s) != 0) { printf("  %s: open failed\n", s->name); return; }
    long n = s->count(s);
    printf("  backend=%-6s resources=%ld\n", s->name, n);
    long show = n < maxshow ? n : maxshow;
    for (long i = 0; i < show; i++) {
        char name[256]; long off = 0, len = 0;
        if (s->stat(s, i, name, sizeof name, &off, &len) != 0) continue;
        printf("    [%3ld] %-40s off=%-10ld len=%ld\n", i, name, off, len);
    }
    if (n > show) printf("    ... (%ld more)\n", n - show);
}

int main(int argc, char **argv) {
    printf("ForTMR2 source abstraction demo\n");
    const char *dir = argc > 1 ? argv[1] : ".";
    printf("\n-- dir backend: %s --\n", dir);
    efml4a_source *sd = efml4a_source_dir_new(dir);
    if (sd) {
        list_source(sd, 8);
        long idx = sd->find ? sd->find(sd, "README.md") : -1;
        if (idx >= 0) {
            long off, len;
            if (sd->stat(sd, idx, NULL, 0, &off, &len) == 0) {
                uint8_t *data = NULL;
                long got = efml4a_read_all(sd, off, len, &data);
                if (got > 0) { printf("    read README.md: %ld bytes\n", got); free(data); }
            }
        }
        efml4a_source_free(sd);
    }

    printf("\n-- mem backend --\n");
    static uint8_t mem[4096];
    for (int i = 0; i < 4096; i++) mem[i] = (uint8_t)(i & 0xFF);
    efml4a_source *sm = efml4a_source_mem_new(mem, sizeof mem, 0);
    if (sm) {
        list_source(sm, 3);
        uint8_t probe[4] = {0xDE, 0xAD, 0xBE, 0xEF};
        long w = sm->write(sm, 100, 4, probe);
        uint8_t back[4];
        long r = sm->read(sm, 100, 4, back);
        printf("    write/read: %ld/%ld %s\n", w, r,
               (w == 4 && r == 4 && !memcmp(probe, back, 4)) ? "ok" : "FAIL");
        efml4a_source_free(sm);
    }
    printf("\ndone\n");
    return 0;
}
