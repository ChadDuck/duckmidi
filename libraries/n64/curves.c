#include "curves.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* Tolerance ladders, from exact to the audible caps. */
static const double volume_db[ES_CURVE_STEPS] = {0, .1, .2, .35, .5, .75, 1};
static const double pan_steps[ES_CURVE_STEPS] = {0, 1, 1, 2, 3, 4, 6};
static const double bend_cents[ES_CURVE_STEPS] = {0, 1, 2, 3, 5, 7, 10};
static const uint64_t spacings[ES_CURVE_SPACINGS] = {1, 2, 3, 4, 6, 8};

typedef struct {
    uint32_t id;
    uint8_t kind;
    size_t *points, count, capacity;
    size_t kept[ES_CURVE_STEPS];
    unsigned step;
} Curve;

/* Volume in decibels; anything quieter than -40 dB counts as -40. */
static double decibels(int32_t value) {
    return fmax(-40, 20 * log10((value > 0 ? value : 1) / 127.0));
}

static double error(uint8_t kind, int32_t a, int32_t b) {
    return kind == ES_CURVE_VOLUME ? fabs(decibels(a) - decibels(b)) : fabs((double)a - b);
}

static double tolerance(uint8_t kind, unsigned step) {
    return kind == ES_CURVE_VOLUME ? volume_db[step]
           : kind == ES_CURVE_PAN  ? pan_steps[step]
                                   : bend_cents[step];
}

/* Simplifies one curve at `step`; marks keep[] when given and returns the changes kept. */
static size_t simplify(const EsCurvePoint *p, const Curve *c, unsigned step, uint64_t spacing,
                       uint64_t settle, bool *keep) {
    size_t kept = 0;
    bool held = false;
    int32_t value = 0;
    uint64_t tick = 0;
    double limit = tolerance(c->kind, step);

    for (size_t j = 0; j < c->count; j++) {
        const EsCurvePoint *point = &p[c->points[j]];
        bool settles = j + 1 == c->count || p[c->points[j + 1]].tick - point->tick >= settle;
        bool moved = !held || error(c->kind, value, point->value) > limit;
        bool spaced = !held || point->tick - tick >= spacing;

        if ((moved && spaced) || (settles && held && point->value != value)) {
            held = true;
            value = point->value;
            tick = point->tick;
            kept++;

            if (keep) {
                keep[c->points[j]] = true;
            }
        }
    }

    return kept;
}

size_t es_thin_curves(const EsCurvePoint *p, size_t count, size_t budget, unsigned spacing,
                      uint64_t settle, bool *keep) {
    Curve *curves = NULL;
    size_t curve_count = 0, curve_capacity = 0, fixed = 0;
    uint64_t gap = spacings[spacing < ES_CURVE_SPACINGS ? spacing : ES_CURVE_SPACINGS - 1];
    bool ok = true;

    for (size_t i = 0; i < count && ok; i++) {
        keep[i] = p[i].kind == ES_CURVE_FIXED;

        if (keep[i]) {
            fixed++;
            continue;
        }

        size_t c = 0;

        while (c < curve_count && curves[c].id != p[i].curve) {
            c++;
        }

        if (c == curve_count) {
            if (curve_count == curve_capacity) {
                curve_capacity = curve_capacity ? curve_capacity * 2 : 16;
                Curve *grown = realloc(curves, curve_capacity * sizeof(*curves));

                if (!grown) {
                    ok = false;
                    break;
                }

                curves = grown;
            }

            curves[curve_count++] = (Curve){.id = p[i].curve, .kind = p[i].kind};
        }

        Curve *curve = &curves[c];

        if (curve->count == curve->capacity) {
            curve->capacity = curve->capacity ? curve->capacity * 2 : 64;
            size_t *grown = realloc(curve->points, curve->capacity * sizeof(size_t));

            if (!grown) {
                ok = false;
                break;
            }

            curve->points = grown;
        }

        curve->points[curve->count++] = i;
    }

    size_t total = fixed;

    for (size_t c = 0; c < curve_count && ok; c++) {
        for (unsigned step = 0; step < ES_CURVE_STEPS; step++) {
            curves[c].kept[step] = simplify(p, &curves[c], step, gap, settle, NULL);
        }

        total += curves[c].kept[0];
    }

    /* Loosen the densest curve first until the changes fit the budget or every curve is at its
       cap; sparse, expressive curves stay exact as long as possible. */
    while (ok && total > budget) {
        size_t densest = curve_count;

        for (size_t c = 0; c < curve_count; c++) {
            if (curves[c].step + 1 < ES_CURVE_STEPS &&
                (densest == curve_count ||
                 curves[c].kept[curves[c].step] > curves[densest].kept[curves[densest].step])) {
                densest = c;
            }
        }

        if (densest == curve_count) {
            break;
        }

        Curve *c = &curves[densest];
        total -= c->kept[c->step];
        c->step++;
        total += c->kept[c->step];
    }

    for (size_t c = 0; c < curve_count && ok; c++) {
        simplify(p, &curves[c], curves[c].step, gap, settle, keep);
    }

    for (size_t c = 0; c < curve_count; c++) {
        free(curves[c].points);
    }

    free(curves);
    return ok ? total : SIZE_MAX;
}

int es_fit_curves(const EsCurvePoint *p, size_t count, uint64_t bytes, uint64_t limit,
                  uint64_t settle, EsCurveRender render, void *user, bool *keep) {
    if (!count) {
        return 0;
    }

    /* hi: the fewest changes known not to fit; lo: the most known to fit. */
    size_t hi_kept = count, lo_kept = 0, lo_budget = 0;
    uint64_t hi_bytes = bytes, lo_bytes = 0;
    unsigned spacing = 0, lo_spacing = 0;
    bool found = false, current = false;
    double per_change = (double)bytes / (double)count;

    for (unsigned pass = 0; pass < 40; pass++) {
        double wanted;

        if (!found) {
            /* Aim a little under the limit; the first estimate charges every byte to the
               curves, so it removes too little rather than too much. */
            wanted = (double)hi_kept - ((double)hi_bytes - (double)limit * .99) / per_change;
        } else if (hi_kept - lo_kept <= (lo_kept / 100 > 8 ? lo_kept / 100 : 8)) {
            break;
        } else if (hi_bytes > lo_bytes && limit > lo_bytes) {
            wanted = (double)lo_kept + ((double)limit - (double)lo_bytes) *
                                           (double)(hi_kept - lo_kept) /
                                           ((double)hi_bytes - (double)lo_bytes);
        } else {
            wanted = ((double)lo_kept + (double)hi_kept) / 2; /* Size says little: bisect. */
        }

        size_t budget = wanted < 1 ? 0 : (size_t)wanted;
        budget = budget >= hi_kept ? hi_kept - 1 : budget;
        budget = found && budget <= lo_kept ? lo_kept + 1 : budget;
        size_t kept = es_thin_curves(p, count, budget, spacing, settle, keep);

        if (kept == SIZE_MAX) {
            return -1;
        }

        if (found && (kept <= lo_kept || kept >= hi_kept)) {
            break;
        }

        uint64_t size = 0;
        int fits = render(user, keep, &size);

        if (fits < 0) {
            return -1;
        }

        current = fits > 0;

        if (fits) {
            found = true;
            lo_kept = kept;
            lo_bytes = size;
            lo_budget = budget;
            lo_spacing = spacing;
            continue;
        }

        /* At the audible caps for this spacing: allow changes further apart. */
        if (!found && kept > budget && ++spacing == ES_CURVE_SPACINGS) {
            return 0;
        }

        if (kept < hi_kept && size < hi_bytes) {
            per_change = (double)(hi_bytes - size) / (double)(hi_kept - kept);
            hi_kept = kept;
            hi_bytes = size;
        }
    }

    if (!found) {
        return 0;
    }

    if (!current) {
        uint64_t size = 0;

        if (es_thin_curves(p, count, lo_budget, lo_spacing, settle, keep) == SIZE_MAX) {
            return -1;
        }

        int fits = render(user, keep, &size);

        if (fits <= 0) {
            return fits < 0 ? -1 : 0;
        }
    }

    return 1;
}
