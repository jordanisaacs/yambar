#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wordexp.h>

#define LOG_MODULE "icon"
#define LOG_ENABLE_DBG 1
#include "icon.h"
#include "log.h"
#include "particle.h"
#include "plugin.h"
#include "stringop.h"

void
icon_pixmaps_free(const struct ref *ref)
{
    struct icon_pixmaps *p = (struct icon_pixmaps *)ref;
    tll_free_and_free(p->list, free);
    free(p);
}

void
icon_pixmaps_dec(struct icon_pixmaps *p)
{
    ref_dec(&p->refcount);
}

struct icon_pixmaps *
icon_pixmaps_inc(struct icon_pixmaps *p)
{
    ref_inc(&p->refcount);
    return p;
}

struct icon_pixmaps *
new_icon_pixmaps()
{
    struct icon_pixmaps *p = malloc(sizeof(*p));
    p->refcount = (struct ref){icon_pixmaps_free, 1};
    icon_pixmaps_t tmp = tll_init();
    p->list = tmp;
    return p;
}

bool
dir_exists(char *path)
{
    struct stat sb;
    return stat(path, &sb) == 0 && S_ISDIR(sb.st_mode);
}

static void
destroy_theme(struct icon_theme *theme)
{
    if (!theme) {
        return;
    }
    free(theme->name);
    free(theme->comment);
    tll_free_and_free(theme->inherits, free);
    tll_free_and_free(theme->directories, free);
    free(theme->dir);

    tll_foreach(theme->subdirs, it)
    {
        free(it->item->name);
        free(it->item);
        tll_remove(theme->subdirs, it);
    }
    free(theme);
}

void
basedirs_free(const struct ref *ref)
{
    struct basedirs *b = (struct basedirs *)ref;
    tll_free_and_free(b->basedirs, free);
    free(b);
}

void
basedirs_dec(struct basedirs *b)
{
    ref_dec(&b->refcount);
}

struct basedirs *
basedirs_inc(struct basedirs *p)
{
    ref_inc(&p->refcount);
    return p;
}

struct basedirs *
basedirs_new(void)
{
    string_list_t basedirs = tll_init();

    struct basedirs* out = malloc(sizeof(*out));
    out->basedirs = basedirs;
    out->refcount = (struct ref){basedirs_free, 1};
    return out;
}


struct basedirs*
get_basedirs(void)
{
    string_list_t basedirs = tll_init();

    tll_push_back(basedirs, strdup("$HOME/.icons")); // deprecated

    char *data_home = getenv("XDG_DATA_HOME");
    tll_push_back(basedirs, strdup(data_home && *data_home ? "$XDG_DATA_HOME/icons" : "$HOME/.local/share/icons"));

    tll_push_back(basedirs, strdup("/usr/share/pixmaps"));

    char *data_dirs = getenv("XDG_DATA_DIRS");
    if (!(data_dirs && *data_dirs)) {
        data_dirs = "/usr/local/share:/usr/share";
    }
    data_dirs = strdup(data_dirs);
    char *dir = strtok(data_dirs, ":");
    do {
        char *path = format_str("%s/icons", dir);
        tll_push_back(basedirs, path);
    } while ((dir = strtok(NULL, ":")));
    free(data_dirs);

    string_list_t basedirs_expanded = tll_init();
    tll_foreach(basedirs, it)
    {
        wordexp_t p;
        if (wordexp(it->item, &p, WRDE_UNDEF) == 0) {
            if (dir_exists(p.we_wordv[0])) {
                tll_push_back(basedirs_expanded, strdup(p.we_wordv[0]));
            }
            wordfree(&p);
        }
    };

    tll_free_and_free(basedirs, free);
    struct basedirs* out = malloc(sizeof(*out));
    out->basedirs = basedirs_expanded;
    out->refcount = (struct ref){basedirs_free, 1};
    return out;
}

static const char *
group_handler(char *old_group, char *new_group, struct icon_theme *theme)
{
    if (!old_group) {
        return new_group && strcasecmp(new_group, "icon theme") == 0 ? NULL : "first group must be 'Icon Theme'";
    }

    if (strcasecmp(old_group, "icon theme") == 0) {
        if (!theme->name) {
            return "missing required key 'Name'";
        } else if (!theme->comment) {
            return "missing required key 'Comment'";
        } else if (tll_length(theme->directories) == 0) {
            return "missing required key 'Directories'";
        }
    } else {
        if (tll_length(theme->subdirs) == 0) { // skip
            return NULL;
        }

        struct icon_theme_subdir *subdir = tll_back(theme->subdirs);
        if (!subdir->size) {
            return "missing required key 'Size'";
        }

        switch (subdir->type) {
        case ICON_DIR_FIXED:
            subdir->max_size = subdir->min_size = subdir->size;
            break;
        case ICON_DIR_SCALABLE: {
            if (!subdir->max_size)
                subdir->max_size = subdir->size;
            if (!subdir->min_size)
                subdir->min_size = subdir->size;
            break;
        }
        case ICON_DIR_THRESHOLD:
            subdir->max_size = subdir->size + subdir->threshold;
            subdir->min_size = subdir->size - subdir->threshold;
        }
    }

    if (new_group) {
        bool found = false;
        tll_foreach(theme->directories, it)
        {
            if (strcmp(it->item, new_group)) {
                found = true;
                break;
            }
        }

        if (found) {
            struct icon_theme_subdir *subdir = calloc(1, sizeof(struct icon_theme_subdir));
            if (!subdir) {
                return "out of memory";
            }
            subdir->name = strdup(new_group);
            subdir->threshold = 2;
            tll_push_back(theme->subdirs, subdir);
        }
    }

    return NULL;
}

string_list_t
split_string(const char *str, const char *delims)
{
    string_list_t res = tll_init();
    char *copy = strdup(str);

    char *token = strtok(copy, delims);
    while (token) {
        tll_push_back(res, strdup(token));
        token = strtok(NULL, delims);
    }
    free(copy);
    return res;
}

static const char *
entry_handler(char *group, char *key, char *value, struct icon_theme *theme)
{
    if (strcmp(group, "Icon Theme") == 0) {
        if (strcmp(key, "Name") == 0) {
            theme->name = strdup(value);
        } else if (strcmp(key, "Comment") == 0) {
            theme->comment = strdup(value);
        } else if (strcmp(key, "Inherits") == 0) {
            theme->inherits = split_string(value, ",");
        } else if (strcmp(key, "Directories") == 0) {
            theme->directories = split_string(value, ",");
        } // Ignored: ScaledDirectories, Hidden, Example
    } else {
        if (tll_length(theme->subdirs) == 0) { // skip
            return NULL;
        }

        struct icon_theme_subdir *subdir = tll_back(theme->subdirs);
        if (strcmp(subdir->name, group) != 0) { // skip
            return NULL;
        }

        if (strcmp(key, "Context") == 0) {
            return NULL; // ignored, but explicitly handled to not fail parsing
        } else if (strcmp(key, "Type") == 0) {
            if (strcmp(value, "Fixed") == 0) {
                subdir->type = ICON_DIR_FIXED;
            } else if (strcmp(value, "Scalable") == 0) {
                subdir->type = ICON_DIR_SCALABLE;
            } else if (strcmp(value, "Threshold") == 0) {
                subdir->type = ICON_DIR_THRESHOLD;
            } else {
                return "ignoring unrecognized icon theme directory type";
            }
            return NULL;
        }

        char *end;
        int n = strtol(value, &end, 10);
        if (*end != '\0') {
            return "invalid value - expected a number";
        }

        if (strcmp(key, "Size") == 0) {
            subdir->size = n;
        } else if (strcmp(key, "MaxSize") == 0) {
            subdir->max_size = n;
        } else if (strcmp(key, "MinSize") == 0) {
            subdir->min_size = n;
        } else if (strcmp(key, "Threshold") == 0) {
            subdir->threshold = n;
        } else if (strcmp(key, "Scale") == 0) {
            subdir->scale = n;
        }
    }
    return NULL;
}

/*
 * This is a Freedesktop Desktop Entry parser (essentially INI)
 * It calls entry_handler for every entry
 * and group_handler between every group (as well as at both ends)
 * Handlers return whether an error occurred, which stops parsing
 */
struct icon_theme *
read_theme_file(char *basedir, char *theme_name)
{
    struct icon_theme *theme = calloc(1, sizeof(struct icon_theme));
    if (!theme) {
        return NULL;
    }
    string_list_t tmp1 = tll_init();
    subdirs_t tmp3 = tll_init();
    theme->directories = tmp1;
    theme->inherits = tmp1;
    theme->subdirs = tmp3;

    // look for index.theme file
    char *path = format_str("%s/%s/index.theme", basedir, theme_name);
    FILE *theme_file = fopen(path, "r");
    free(path);
    if (!theme_file) {
        free(theme);
        return NULL;
    }

    string_list_t groups = tll_init();
    char *full_line = NULL;
    const char *error = NULL;
    int line_no = 0;
    while (true) {
        const char *warning = NULL;
        size_t sz = 0;
        ssize_t nread = getline(&full_line, &sz, theme_file);
        if (nread == -1) {
            break;
        }

        ++line_no;

        char *line = full_line - 1;
        while (isspace(*++line)) {
        } // remove leading whitespace
        if (!*line || line[0] == '#')
            continue; // ignore blank lines & comments

        int len = nread - (line - full_line);
        while (isspace(line[--len])) {
        }
        line[++len] = '\0'; // Remove trailing whitespace

        if (line[0] == '[') { // Group handler
            // check well-formed
            int i = 1;
            for (; !iscntrl(line[i]) && line[i] != '[' && line[i] != ']'; ++i) {
            }
            if (i != --len || line[i] != ']') {
                warning = "malformed group header";
                goto warn;
            }

            line[len] = '\0';
            line = &line[1];

            tll_foreach(groups, it)
            {
                // If duplicate move to back and continue
                //
                if (strcmp(it->item, line) == 0) {
                    tll_push_back(groups, it->item);
                    tll_remove(groups, it);
                    goto next;
                }
            }

            char *last_group = tll_length(groups) ? tll_back(groups) : NULL;
            error = group_handler(last_group, line, theme);
            if (error) {
                break;
            }

            tll_push_back(groups, strdup(line));
        } else {
            const char *warning = NULL;
            if (tll_length(groups) == 0) {
                error = "unexpected content before first header";
                break;
            }

            // check well-formed
            int eok = 0;
            for (; isalnum(line[eok]) || line[eok] == '-'; ++eok) {
            }
            int i = eok - 1;
            while (isspace(line[++i])) {
            }
            if (line[i] == '[') {
                // not handling localized values:
                // https://specifications.freedesktop.org/desktop-entry-spec/latest/ar01s05.html
                //
                continue;
            }
            if (line[i] != '=') {
                warning = "malformed key-value pair";
                goto warn;
            }

            line[eok] = '\0'; // split into key-value pair

            char *value = &line[i];
            while (isspace(*++value)) {
            }

            warning = entry_handler(tll_back(groups), line, value, theme);
            if (warning) {
                goto warn;
            }

            continue;
        }

    next:
        continue;

    warn:;
        assert(warning != NULL);
        char *group = tll_length(groups) > 0 ? tll_back(groups) : "n/a";
        LOG_INFO("Error during load of theme '%s' - parsing of file "
                 "'%s/%s/index.theme' encountered '%s' on line %d (group '%s') - continuing",
                 theme_name, basedir, theme_name, warning, line_no, group);
    }

    if (!error && tll_length(groups) == 0) {
        error = "empty file";
    }

    if (error) {
        char *group = tll_length(groups) > 0 ? tll_back(groups) : "n/a";
        LOG_WARN("Failed to load theme '%s' - parsing of file "
                 "'%s/%s/index.theme' failed on line %d (group '%s'): %s",
                 theme_name, basedir, theme_name, line_no, group, error);
        destroy_theme(theme);
        theme = NULL;
    } else {
        theme->dir = strdup(theme_name);
    }

    free(full_line);
    tll_free_and_free(groups, free);
    return theme;
}

themes_t
load_themes_in_dir(char *basedir)
{
    themes_t themes = tll_init();

    DIR *dir;
    if (!(dir = opendir(basedir))) {
        return themes;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;

        struct icon_theme *theme = read_theme_file(basedir, entry->d_name);
        if (theme) {
            tll_push_back(themes, theme);
        }
    }
    closedir(dir);
    return themes;
}

void
log_loaded_themes(themes_t themes)
{
    if (tll_length(themes) == 0) {
        LOG_INFO("Warning: no icon themes loaded");
        return;
    }
    const char sep[] = ", ";
    size_t sep_len = strlen(sep);

    size_t len = 0;
    tll_foreach(themes, it) { len += strlen(it->item->name) + sep_len; }

    char *str = malloc(len + 1);
    if (!str) {
        return;
    }
    char *p = str;
    bool start = true;
    tll_foreach(themes, it)
    {
        if (!start) {
            memcpy(p, sep, sep_len);
            p += sep_len;
        }
        start = false;

        struct icon_theme *theme = it->item;
        size_t name_len = strlen(theme->name);
        memcpy(p, theme->name, name_len);
        p += name_len;
    }
    *p = '\0';

    LOG_INFO("Loaded icon themes: %s", str);
    free(str);
}

void
themes_free(const struct ref *ref)
{
    LOG_INFO("themes free");
    struct themes *p = (struct themes *)ref;
    tll_free_and_free(p->themes, destroy_theme);
    free(p);
}

void
themes_dec(struct themes *t)
{
    ref_dec(&t->refcount);
}

struct themes *
themes_inc(struct themes *t)
{
    ref_inc(&t->refcount);
    return t;
}

struct themes *
init_themes(struct basedirs *basedirs)
{

    themes_t themes = tll_init();
    tll_foreach(basedirs->basedirs, it)
    {
        themes_t dir_themes = load_themes_in_dir(it->item);
        tll_foreach(dir_themes, it)
        {
            struct icon_theme *theme = it->item;
            tll_remove(dir_themes, it);
            tll_push_back(themes, theme);
        }
    }

    log_loaded_themes(themes);
    struct themes *out = malloc(sizeof(*out));
    out->themes = themes;
    out->refcount = (struct ref){themes_free, 1};
    return out;
}

static char *
find_icon_in_subdir(char *name, char *basedir, char *theme, char *subdir)
{
    static const char *extensions[] = {
        "svg",
        "png",
    };

    for (size_t i = 0; i < sizeof(extensions) / sizeof(*extensions); ++i) {
        char *path = format_str("%s/%s/%s/%s.%s", basedir, theme, subdir, name, extensions[i]);
        if (path && access(path, R_OK) == 0) {
            return path;
        }
        free(path);
    }

    return NULL;
}

static bool
theme_exists_in_basedir(char *theme, char *basedir)
{
    char *path = format_str("%s/%s", basedir, theme);
    bool ret = dir_exists(path);
    free(path);
    return ret;
}

static char *
find_icon_with_theme(string_list_t basedirs, themes_t themes, char *name, int size, char *theme_name, int *min_size,
                     int *max_size)
{
    struct icon_theme *theme = NULL;
    tll_foreach(themes, it)
    {
        theme = it->item;
        if (strcmp(theme->name, theme_name) == 0) {
            break;
        }
        theme = NULL;
    }
    if (!theme)
        return NULL;

    char *icon = NULL;
    tll_foreach(basedirs, bd_it)
    {
        if (!theme_exists_in_basedir(theme->dir, bd_it->item)) {
            LOG_INFO("theme doesn't exist");
            continue;
        }

        tll_rforeach(theme->subdirs, sd_it)
        {
            struct icon_theme_subdir *subdir = sd_it->item;
            if (size >= subdir->min_size && size <= subdir->max_size) {
                if ((icon = find_icon_in_subdir(name, bd_it->item, theme->dir, subdir->name))) {
                    *min_size = subdir->min_size;
                    *max_size = subdir->max_size;
                    return icon;
                }
            }
        }
    }

    // inexact match
    unsigned smallest_error = -1; // UINT_MAX
    tll_foreach(basedirs, bd_it)
    {
        if (!theme_exists_in_basedir(theme->dir, bd_it->item)) {
            continue;
        }

        tll_rforeach(theme->subdirs, sd_it)
        {
            struct icon_theme_subdir *subdir = sd_it->item;
            unsigned error = (size > subdir->max_size ? size - subdir->max_size : 0)
                             + (size < subdir->min_size ? subdir->min_size - size : 0);
            if (error < smallest_error) {
                char *test_icon = find_icon_in_subdir(name, bd_it->item, theme->dir, subdir->name);
                if (test_icon) {
                    icon = test_icon;
                    smallest_error = error;
                    *min_size = subdir->min_size;
                    *max_size = subdir->max_size;
                }
            }
        }
    }

    if (!icon) {
        tll_foreach(theme->inherits, it)
        {
            icon = find_icon_with_theme(basedirs, themes, name, size, it->item, min_size, max_size);
            if (icon) {
                break;
            }
        }
    }

    return icon;
}

static char *
find_fallback_icon(string_list_t basedirs, char *name, int *min_size, int *max_size)
{
    tll_foreach(basedirs, it)
    {
        char *icon = find_icon_in_subdir(name, it->item, "", "");
        if (icon) {
            *min_size = 1;
            *max_size = 512;
            return icon;
        }
    }
    return NULL;
}

char *
find_icon(themes_t themes, string_list_t basedirs, char *name, int size, char *theme, int *min_size, int *max_size)
{
    // TODO https://specifications.freedesktop.org/icon-theme-spec/icon-theme-spec-latest.html#implementation_notes
    //
    char *icon = NULL;
    if (theme) {
        icon = find_icon_with_theme(basedirs, themes, name, size, theme, min_size, max_size);
    }
    if (!icon && !(theme && strcmp(theme, "Hicolor") == 0)) {
        icon = find_icon_with_theme(basedirs, themes, name, size, "Hicolor", min_size, max_size);
    }
    if (!icon) {
        icon = find_fallback_icon(basedirs, name, min_size, max_size);
    }
    return icon;
}
