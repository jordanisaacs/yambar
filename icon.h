#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <tllist.h>

struct ref {
    void (*free)(const struct ref *);
    atomic_size_t count;
};

static inline void
ref_inc(const struct ref *ref)
{
    atomic_fetch_add((atomic_size_t *)&ref->count, 1);
}

static inline void
ref_dec(const struct ref *ref)
{
    if (atomic_fetch_sub((atomic_size_t *)&ref->count, 1) == 1) {
        ref->free(ref);
    }
}

struct icon_pixmap {
    int size;
    unsigned char pixels[];
};

struct icon_pixmaps *new_icon_pixmaps();

void icon_pixmaps_dec(struct icon_pixmaps *p);

struct icon_pixmaps *icon_pixmaps_inc(struct icon_pixmaps *ip);

typedef tll(struct icon_pixmap *) icon_pixmaps_t;

struct icon_pixmaps {
    struct ref refcount;
    icon_pixmaps_t list;
};

enum icon_dir_type { ICON_DIR_FIXED, ICON_DIR_SCALABLE, ICON_DIR_THRESHOLD };

struct icon_theme_subdir {
    char *name;
    char *context;

    int size;
    int max_size;
    int min_size;
    int scale;
    int threshold;
    enum icon_dir_type type;
};

typedef tll(struct icon_theme *) themes_t;
typedef tll(char *) string_list_t;
typedef tll(struct icon_theme_subdir *) subdirs_t;

struct themes {
    struct ref refcount;
    themes_t themes;
};

struct basedirs {
    struct ref refcount;
    string_list_t basedirs;
};

struct icon_theme {
    char *name;
    char *comment;
    string_list_t inherits;    // char *
    string_list_t directories; // char *

    char *dir;
    subdirs_t subdirs; // struct icon_theme_subdir *
};

bool dir_exists(char *path);

struct basedirs *get_basedirs(void);

struct themes *init_themes(struct basedirs *basedirs);

void basedirs_dec(struct basedirs *p);

struct basedirs *basedirs_inc(struct basedirs *p);

struct basedirs *basedirs_new(void);

void themes_dec(struct themes *p);

struct themes *themes_inc(struct themes *p);

char *find_icon(themes_t themes, string_list_t basedirs, char *name, int size, char *theme, int *min_size,
                int *max_size);

void log_loaded_themes(themes_t themes);
