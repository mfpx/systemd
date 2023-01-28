/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include "build.h"
#include "fd-util.h"
#include "fileio.h"
#include "hashmap.h"
#include "manager-dump.h"
#include "unit-serialize.h"

void manager_dump_jobs(Manager *s, FILE *f, char **patterns, const char *prefix) {
        Job *j;

        assert(s);
        assert(f);

        HASHMAP_FOREACH(j, s->jobs) {

                if (!strv_fnmatch_or_empty(patterns, j->unit->id, FNM_NOESCAPE))
                        continue;

                job_dump(j, f, prefix);
        }
}

void manager_dump_units(Manager *s, FILE *f, char **patterns, const char *prefix) {
        Unit *u;
        const char *t;

        assert(s);
        assert(f);

        HASHMAP_FOREACH_KEY(u, t, s->units) {
                if (u->id != t)
                        continue;

                if (!strv_fnmatch_or_empty(patterns, u->id, FNM_NOESCAPE))
                        continue;

                unit_dump(u, f, prefix);
        }
}

static void manager_dump_header(Manager *m, FILE *f, const char *prefix) {

        /* NB: this is a debug interface for developers. It's not supposed to be machine readable or be
         * stable between versions. We take the liberty to restructure it entirely between versions and
         * add/remove fields at will. */

        fprintf(f, "%sManager: systemd " STRINGIFY(PROJECT_VERSION) " (" GIT_VERSION ")\n", strempty(prefix));
        fprintf(f, "%sFeatures: %s\n", strempty(prefix), systemd_features);

        for (ManagerTimestamp q = 0; q < _MANAGER_TIMESTAMP_MAX; q++) {
                const dual_timestamp *t = m->timestamps + q;

                if (dual_timestamp_is_set(t))
                        fprintf(f, "%sTimestamp %s: %s\n",
                                strempty(prefix),
                                manager_timestamp_to_string(q),
                                timestamp_is_set(t->realtime) ? FORMAT_TIMESTAMP(t->realtime) :
                                                                FORMAT_TIMESPAN(t->monotonic, 1));
        }
}

void manager_dump(Manager *m, FILE *f, char **patterns, const char *prefix) {
        assert(m);
        assert(f);

        /* If no pattern is provided, dump the full manager state including the manager version, features and
         * so on. Otherwise limit the dump to the units/jobs matching the specified patterns. */
        if (!patterns)
                manager_dump_header(m, f, prefix);

        manager_dump_units(m, f, patterns, prefix);
        manager_dump_jobs(m, f, patterns, prefix);
}

int manager_get_dump_string(Manager *m, char **patterns, char **ret) {
        _cleanup_free_ char *dump = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        size_t size;
        int r;

        assert(m);
        assert(ret);

        f = open_memstream_unlocked(&dump, &size);
        if (!f)
                return -errno;

        manager_dump(m, f, patterns, NULL);

        r = fflush_and_check(f);
        if (r < 0)
                return r;

        f = safe_fclose(f);

        *ret = TAKE_PTR(dump);

        return 0;
}

void manager_test_summary(Manager *m) {
        assert(m);

        printf("-> By units:\n");
        manager_dump_units(m, stdout, /* patterns= */ NULL, "\t");

        printf("-> By jobs:\n");
        manager_dump_jobs(m, stdout, /* patterns= */ NULL, "\t");
}
