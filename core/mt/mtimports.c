// mtimports: list a set of i386 images' imports that have no host implementation.
#include "mt_internal.h"
#include <stdio.h>
int main(int argc, char **argv) {
    char err[256];
    Mt *m = mt_create(err, sizeof err);
    for (int i = 1; i < argc; i++) mt_load(m, argv[i]);
    mt_bind_all(m);
    for (int i = 0; i < m->ntraps; i++) if (!m->trap_fn[i]) printf("%s\n", m->trap_name[i]);
    return 0;
}
