#include "ramp.h"

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include <stdio.h>

struct private {
    char *tag;
    struct particle **particles;
    size_t count;
};

struct eprivate {
    struct exposable *exposable;
};

static void
exposable_destroy(struct exposable *exposable)
{
    struct eprivate *e = exposable->private;
    e->exposable->destroy(e->exposable);

    free(e);
    exposable_default_destroy(exposable);
}

static int
begin_expose(struct exposable *exposable, cairo_t *cr)
{
    struct eprivate *e = exposable->private;

    exposable->width = (
        exposable->particle->left_margin +
        e->exposable->begin_expose(e->exposable, cr) +
        exposable->particle->right_margin);

    return exposable->width;
}

static void
expose(const struct exposable *exposable, cairo_t *cr, int x, int y, int height)
{
    const struct deco *deco = exposable->particle->deco;
    if (deco != NULL)
        deco->expose(deco, cr, x, y, exposable->width, height);

    struct eprivate *e = exposable->private;
    e->exposable->expose(
        e->exposable, cr, x + exposable->particle->left_margin, y, height);
}

static void
particle_destroy(struct particle *particle)
{
    struct private *p = particle->private;

    for (size_t i = 0; i < p->count; i++)
        p->particles[i]->destroy(p->particles[i]);

    free(p->tag);
    free(p->particles);
    free(p);
    particle_default_destroy(particle);
}

static struct exposable *
instantiate(const struct particle *particle, const struct tag_set *tags)
{
    const struct private *p = particle->private;
    const struct tag *tag = tag_for_name(tags, p->tag);
    assert(tag != NULL);

    assert(p->count > 0);

    long value = tag->as_int(tag);
    long min = tag->min(tag);
    long max = tag->max(tag);

    assert(value >= min && value <= max);
    assert(max >= min);

    size_t idx = 0;
    if (max - min > 0)
        idx = p->count * value / (max - min);

    if (idx == p->count)
        idx--;
    /*
     * printf("ramp: value: %lu, min: %lu, max: %lu, progress: %f, idx: %zu\n",
     *        value, min, max, progress, idx);
     */
    assert(idx >= 0 && idx < p->count);

    struct particle *pp = p->particles[idx];

    struct eprivate *e = malloc(sizeof(*e));
    e->exposable = pp->instantiate(pp, tags);

    char *on_click = tags_expand_template(particle->on_click_template, tags);
    struct exposable *exposable = exposable_common_new(particle, on_click);
    exposable->private = e;
    exposable->destroy = &exposable_destroy;
    exposable->begin_expose = &begin_expose;
    exposable->expose = &expose;

    free(on_click);
    return exposable;
}

struct particle *
particle_ramp_new(const char *tag, struct particle *particles[], size_t count,
                  int left_margin, int right_margin)
{
    struct particle *particle = particle_common_new(
        left_margin, right_margin, NULL);
    particle->destroy = &particle_destroy;
    particle->instantiate = &instantiate;

    struct private *priv = malloc(sizeof(*priv));
    priv->tag = strdup(tag);
    priv->particles = calloc(count, sizeof(priv->particles[0]));
    priv->count = count;

    for (size_t i = 0; i < count; i++)
        priv->particles[i] = particles[i];

    particle->private = priv;
    return particle;
}
