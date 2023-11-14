#include <assert.h>
#include <limits.h>
#include <pixman.h>
#include <stdlib.h>
#include <string.h>

#define LOG_MODULE "icon"
#define LOG_ENABLE_DBG 1
#include "../char32.h"
#include "../config-verify.h"
#include "../config.h"
#include "../icon.h"
#include "../log.h"
#include "../particle.h"
#include "../plugin.h"
#include "../png-yambar.h"
#include "../stride.h"
#include "../svg.h"
#include "../tag.h"

struct private
{
    char *text;
    bool use_tag;
};

struct eprivate {
    pixman_image_t *image;
    bool is_png;
};

static void
exposable_destroy(struct exposable *exposable)
{
    struct eprivate *e = exposable->private;

    if (e->image) {
        pixman_image_unref(e->image);
    }

    exposable_default_destroy(exposable);
}

static int
begin_expose(struct exposable *exposable)
{
    struct eprivate *e = exposable->private;

    if (!e->image) {
        exposable->width = 0;
    } else {
        exposable->width = exposable->particle->right_margin + exposable->particle->left_margin;
        exposable->width += exposable->particle->icon_size;
    }

    return exposable->width;
}

static void
expose(const struct exposable *exposable, pixman_image_t *pix, int x, int y, int height)
{
    exposable_render_deco(exposable, pix, x, y, height);

    const struct eprivate *e = exposable->private;
    const struct particle *p = exposable->particle;

    if (!e->image) {
        return;
    }

    int target_size = p->icon_size;
    double baseline = (double)y + (double)(height - target_size) / 2.0;
    double src_y = 0;
    double src_x = 0;

    // Pixmaps and PNGs may not be the correct size, ensure crop is
    // centered
    //
    int png_w = pixman_image_get_width(e->image);
    if (png_w != target_size) {
        src_x = (double)(abs(target_size - png_w)) / 2.0;
    }

    int png_h = pixman_image_get_height(e->image);
    if (png_h != target_size) {
        src_y = (double)(abs(target_size - png_h)) / 2.0;
    }

    pixman_image_composite32(PIXMAN_OP_OVER, e->image, NULL, pix, src_x, src_y, 0, 0, x, baseline,
                             target_size, target_size);
}

static struct exposable *
instantiate(const struct particle *particle, const struct tag_set *tags)
{
    struct private *p = (struct private *)particle->private;
    struct eprivate *e = calloc(1, sizeof(*e));
    e->is_png = false;
    e->image = NULL;

    char *name = tags_expand_template(p->text, tags);
    char *icon_name = NULL;
    string_list_t basedirs = tll_init();

    // TODO: An icon cache
    //
    if (p->use_tag) {
        const struct icon_tag *tag = icon_tag_for_name(tags, name);
        LOG_DBG("finding icon tag: %s", name);
        free(name);
        if (!tag) {
            goto out;
        }

        struct icon_pixmaps *pixmaps = tag->pixmaps(tag);
        struct icon_pixmap *icon_pixmap = NULL;
        int min_error = INT_MAX;
        tll_foreach(pixmaps->list, it) {
            int e = abs(particle->icon_size - it->item->size);
            if (e < min_error) {
                icon_pixmap = it->item;
                min_error = e;
            }
        }

        if (icon_pixmap) {
            LOG_DBG("found pixmap");
            e->image = pixman_image_create_bits_no_clear(PIXMAN_a8r8g8b8, icon_pixmap->size, icon_pixmap->size,
                                                (uint32_t *)icon_pixmap->pixels,
                                                stride_for_format_and_width(PIXMAN_a8r8g8b8, icon_pixmap->size));
        }
        // case ICON_TAG_TYPE_NAME: {
        //     icon_name = tag->icon_name(tag);
        //     tll_foreach(particle->basedirs->basedirs, it) { tll_push_back(basedirs, it->item); }
        //     if (tag->search_path(tag)) {
        //         tll_push_back(basedirs, tag->search_path(tag));
        //     }
        //     break;
        // }
    } else {
        icon_name = name;
        tll_foreach(particle->basedirs->basedirs, it) { tll_push_back(basedirs, it->item); }
    }

    if (icon_name) {
        int min_size = 0, max_size = 0;

        char *icon_path = find_icon(particle->themes->themes, particle->basedirs->basedirs, icon_name, particle->icon_size, particle->icon_theme,
                                    &min_size, &max_size);
        if (icon_path) {
            int len = strlen(icon_path);
            if (icon_path[len - 3] == 's') {
                e->image = svg_load(icon_path, particle->icon_size);
            } else if (icon_path[len - 3] == 'p') {
                // TODO: png scaling
                //
                e->image = png_load(icon_path);
            } else {
                LOG_ERR("invalid icon path, not png or svg: %s", icon_path);
            }
        }
        free(icon_path);
    }


out:
    free(icon_name);
    tll_free(basedirs);

    struct exposable *exposable = exposable_common_new(particle, tags);
    exposable->private = e;
    exposable->destroy = &exposable_destroy;
    exposable->begin_expose = &begin_expose;
    exposable->expose = &expose;
    return exposable;
}

static void
particle_destroy(struct particle *particle)
{
    struct private *p = particle->private;
    free(p->text);
    particle_default_destroy(particle);
}

static struct particle *
icon_new(struct particle *common, const char *name, bool use_tag)
{
    struct private *p = calloc(1, sizeof(*p));
    p->text = strdup(name);
    p->use_tag = use_tag;

    common->private = p;
    common->destroy = &particle_destroy;
    common->instantiate = &instantiate;
    return common;
}

static struct particle *
from_conf(const struct yml_node *node, struct particle *common)
{
    const struct yml_node *name = yml_get_value(node, "name");
    const struct yml_node *use_tag_node = yml_get_value(node, "use-tag");

    bool use_tag = false;

    if (use_tag_node)
        use_tag = yml_value_as_bool(use_tag_node);
    LOG_DBG("use tag: %d", use_tag);

    return icon_new(common, yml_value_as_string(name), use_tag);
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        {"name", true, &conf_verify_string},
        {"use-tag", false, &conf_verify_bool},
        PARTICLE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct particle_iface particle_icon_iface = {
    .verify_conf = &verify_conf,
    .from_conf = &from_conf,
};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct particle_iface iface __attribute__((weak, alias("particle_icon_iface")));
#endif
