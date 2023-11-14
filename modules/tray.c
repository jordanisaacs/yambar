#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <icon.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <tllist.h>
#include <wordexp.h>

#define LOG_MODULE "tray"
#define LOG_ENABLE_DBG 1
#include "../log.h"
#include "../particles/dynlist.h"
#include "../plugin.h"
#include "../stringop.h"

static const char *watcher_path = "/StatusNotifierWatcher";
static const char *item_path = "/StatusNotifierItem";

struct sni_slot {
    struct sni *sni;

    const char *prop;
    const char *type;
    void *dest;

    sd_bus_slot *slot;
};

typedef tll(struct sni_slot *) sni_slots_t;

struct sni {
    struct private *m;
    struct particle *template;

    // dbus properties
    char *watcher_id;
    char *service;
    char *path;
    char *interface;

    char *category;
    char *id;
    char *title;
    char *status;
    char *icon_name;
    struct icon_pixmaps *icon_pixmap;
    char *attention_icon_name;
    struct icon_pixmaps *attention_icon_pixmap;
    char *overlay_icon_name;
    struct icon_pixmaps *overlay_icon_pixmap;
    bool item_is_menu;
    char *menu;
    char *icon_theme_path; // nono-standard kde property

    sni_slots_t slots;
};

typedef tll(char *) hosts_t;
typedef tll(char *) items_t;
typedef tll(struct sni *) snis_t;

struct watcher {
    struct private *m;
    char *interface;

    sd_bus *bus;
    hosts_t hosts;
    items_t items;
    int version;
};

struct host {
    struct private *m;

    char *service;
    char *watcher_interface;
};

struct private
{
    struct module *mod;
    struct particle *template;

    int fd;
    sd_bus *bus;

    struct host host_xdg;
    struct host host_kde;
    snis_t items; // struct sni
    struct watcher *watcher_xdg;
    struct watcher *watcher_kde;
};

// utils

// sni item -----------

// static bool
// sni_ready(struct sni *sni)
// {
//     return sni->status
//            && (sni->status[0] == 'N' ? // NeedsAttention
//                    sni->attention_icon_name || tll_length(sni->attention_icon_pixmap) != 0
//                                      : sni->icon_name || tll_length(sni->icon_pixmap) != 0);
// }
//
// static void
// set_sni_dirty(struct sni *sni)
// {
//     if (sni_ready(sni)) {
//         if (sni->m->mod->bar) {
//             sni->m->mod->bar->refresh(sni->m->mod->bar);
//         }
//     }
// }

static int
read_pixmap(sd_bus_message *msg, struct sni *sni, const char *prop, struct icon_pixmaps **dest)
{
    int ret = sd_bus_message_enter_container(msg, 'a', "(iiay)");
    if (ret < 0) {
        LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
        return ret;
    }

    if (sd_bus_message_at_end(msg, 0)) {
        LOG_DBG("[SNI] %s %s no. of icons = 0", sni->watcher_id, prop);
        return ret;
    }

    struct icon_pixmaps *pixmaps = new_icon_pixmaps();

    while (!sd_bus_message_at_end(msg, 0)) {
        ret = sd_bus_message_enter_container(msg, 'r', "iiay");
        if (ret < 0) {
            LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
            goto error;
        }

        int width, height;
        ret = sd_bus_message_read(msg, "ii", &width, &height);
        if (ret < 0) {
            LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
            goto error;
        }

        const void *pixels;
        size_t npixels;
        ret = sd_bus_message_read_array(msg, 'y', &pixels, &npixels);
        if (ret < 0) {
            LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
            goto error;
        }

        if (height > 0 && width == height) {
            LOG_DBG("[SNI] %s %s: found icon w:%d h:%d", sni->watcher_id, prop, width, height);
            struct icon_pixmap *p = malloc(sizeof(*p) + npixels);
            p->size = width;
            // convert from network byte order to host byte order
            //
            for (int i = 0; i < width * height; ++i) {
                ((uint32_t *)p->pixels)[i] = ntohl(((uint32_t *)pixels)[i]);
            }

            tll_push_back(pixmaps->list, p);
        } else {
            LOG_DBG("[SNI] %s %s: discard invalid icon w:%d h:%d", sni->watcher_id, prop, width, height);
        }

        sd_bus_message_exit_container(msg);
    }

    if (tll_length(pixmaps->list) < 1) {
        LOG_DBG("[SNI] %s %s no. of icons = 0", sni->watcher_id, prop);
        goto error;
    }

    if (*dest) {
        icon_pixmaps_dec(*dest);
    }
    *dest = pixmaps;
    LOG_DBG("[SNI] %s %s no. of icons = %zu", sni->watcher_id, prop, tll_length(pixmaps->list));

    return ret;
error:
    icon_pixmaps_dec(pixmaps);
    return ret;
}

static int
get_property_callback(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni_slot *d = data;
    struct sni *sni = d->sni;

    const char *prop = d->prop;
    const char *type = d->type;
    void *dest = d->dest;

    int ret;
    if (sd_bus_message_is_method_error(msg, NULL)) {
        const sd_bus_error *err = sd_bus_message_get_error(msg);
        if ((!strcmp(prop, "IconThemePath")) && (!strcmp(err->name, SD_BUS_ERROR_UNKNOWN_PROPERTY))) {
            LOG_DBG("[SNI]: %s %s: %s", sni->watcher_id, prop, err->message);
        } else {
            LOG_ERR("[SNI]: %s %s: %s", sni->watcher_id, prop, err->message);
        }
        ret = sd_bus_message_get_errno(msg);
        goto cleanup;
    }

    ret = sd_bus_message_enter_container(msg, 'v', type);
    if (ret < 0) {
        LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
        goto cleanup;
    }

    if (!type) {
        ret = read_pixmap(msg, sni, prop, dest);
        if (ret < 0) {
            goto cleanup;
        }
    } else {
        if (*type == 's' || *type == 'o') {
            free(*(char **)dest);
        }

        ret = sd_bus_message_read(msg, type, dest);
        if (ret < 0) {
            LOG_ERR("[SNI] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
            goto cleanup;
        }

        if (*type == 's' || *type == 'o') {
            char **str = dest;
            *str = strdup(*str);
            LOG_DBG("[SNI] %s %s: %s", sni->watcher_id, prop, *str);
        } else if (*type == 'b') {
            LOG_DBG("[SNI] %s %s: %s", sni->watcher_id, prop, *(bool *)dest ? "true" : "false");
        }
    }

    sni->m->mod->bar->refresh(sni->m->mod->bar);

    // if (strcmp(prop, "Status") == 0
    //     || (sni->status && (sni->status[0] == 'N' ? prop[0] == 'A' : strncmp(prop, "Icon", 4) == 0))) {
    //     set_sni_dirty(sni);
    // }
cleanup:
    tll_foreach(sni->slots, it)
    {
        if (it->item == d) {
            tll_remove_and_free(sni->slots, it, free);
            break;
        }
    };
    return ret;
}

static void
sni_get_property_async(struct sni *sni, const char *prop, const char *type, void *dest)
{
    struct sni_slot *data = calloc(1, sizeof(struct sni_slot));
    if (!data) {
        LOG_ERR("Failed to allocate data slot: %s %s", sni->watcher_id, prop);
        return;
    }

    LOG_DBG("get property");

    data->sni = sni;
    data->prop = prop;
    data->type = type;
    data->dest = dest;
    int ret = sd_bus_call_method_async(sni->m->bus, &data->slot, sni->service, sni->path, "org.freedesktop.DBus.Properties",
                                   "Get", get_property_callback, data, "ss", sni->interface, prop);

    if (ret >= 0) {
        tll_push_front(sni->slots, data);
    } else {
        LOG_ERR("[Status Notifier Item] %s %s: %s", sni->watcher_id, prop, strerror(-ret));
        free(data);
    }
}

/*
 * There is a quirk in sd-bus that in some systems, it is unable to get the
 * well-known names on the bus, so it cannot identify if an incoming signal,
 * which uses the sender's unique name, actually matches the callback's matching
 * sender if the callback uses a well-known name, in which case it just calls
 * the callback and hopes for the best, resulting in false positives. In the
 * case of NewIcon & NewAttentionIcon, this doesn't affect anything, but it
 * means that for NewStatus, if the SNI does not definitely match the sender,
 * then the safe thing to do is to query the status independently.
 * This function returns 1 if the SNI definitely matches the signal sender,
 * which is returned by the calling function to indicate that signal matching
 * can stop since it has already found the required callback, otherwise, it
 * returns 0, which allows matching to continue.
 */
static int
sni_check_msg_sender(struct sni *sni, sd_bus_message *msg, const char *signal)
{
    bool has_well_known_names = sd_bus_creds_get_mask(sd_bus_message_get_creds(msg)) & SD_BUS_CREDS_WELL_KNOWN_NAMES;
    if (sni->service[0] == ':' || has_well_known_names) {
        LOG_DBG("%s has new %s", sni->watcher_id, signal);
        return 1;
    } else {
        LOG_DBG("%s may have new %s", sni->watcher_id, signal);
        return 0;
    }
}

static int
handle_new_icon(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni *sni = data;
    LOG_DBG("[Signal Handle] NewIcon: %s", sni->id);
    sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
    sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);
    if (strcmp(sni->interface, "org.kde.StatusNotifierItem") == 0) {
        sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
    }
    int result = sni_check_msg_sender(sni, msg, "icon");
    return result;
}

static int
handle_new_attention_icon(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni *sni = data;
    LOG_DBG("[Signal Handle] NewAttentionIcon: %s", sni->id);
    sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
    sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);
    int result = sni_check_msg_sender(sni, msg, "attention icon");
    return result;
}

static int
handle_new_overlay_icon(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni *sni = data;
    LOG_DBG("[Signal Handle] NewOverlayIcon: %s", sni->id);
    sni_get_property_async(sni, "OverlayIconName", "s", &sni->overlay_icon_name);
    sni_get_property_async(sni, "OverlayIconPixmap", NULL, &sni->overlay_icon_pixmap);
    int result = sni_check_msg_sender(sni, msg, "overlay icon");
    return result;
}

static int
handle_new_status(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni *sni = data;
    int ret = sni_check_msg_sender(sni, msg, "status");
    if (ret == 1) {
        char *status;
        int r = sd_bus_message_read(msg, "s", &status);
        if (r < 0) {
            LOG_ERR("%s new status error: %s", sni->watcher_id, strerror(-ret));
            ret = r;
        } else {
            free(sni->status);
            sni->status = strdup(status);
            LOG_DBG("%s has new status = '%s'", sni->watcher_id, status);
            sni->m->mod->bar->refresh(sni->m->mod->bar);
        }
    } else {
        sni_get_property_async(sni, "Status", "s", &sni->status);
    }

    return ret;
}

static int
handle_new_title(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    struct sni *sni = data;
    sni_get_property_async(sni, "Title", "s", &sni->status);
    return 0;
}

static void
sni_match_signal_async(struct sni *sni, char *signal, sd_bus_message_handler_t callback)
{
    struct sni_slot *slot = calloc(1, sizeof(struct sni));
    int ret = sd_bus_match_signal_async(sni->m->bus, &slot->slot, sni->service, sni->path, sni->interface, signal,
                                        callback, NULL, sni);
    if (ret >= 0) {
        tll_push_front(sni->slots, slot);
    } else {
        LOG_ERR("%s failed to subscribe to signal %s: %s", sni->service, signal, strerror(-ret));
        free(slot);
    }
}

struct sni *
create_sni(struct private *tray, char *id)
{

    struct sni *sni = calloc(1, sizeof(struct sni));
    if (!sni) {
        return NULL;
    }
    sni->m = tray;
    sni_slots_t tmp = tll_init();
    sni->slots = tmp;
    sni->watcher_id = strdup(id);
    char *path_ptr = strchr(id, '/');
    if (!path_ptr) {
        sni->service = strdup(id);
        sni->path = strdup(item_path);
        sni->interface = "org.freedesktop.StatusNotifierItem";
    } else {
        sni->service = strndup(id, path_ptr - id);
        sni->path = strdup(path_ptr);
        sni->interface = "org.kde.StatusNotifierItem";
        sni_get_property_async(sni, "IconThemePath", "s", &sni->icon_theme_path);
    }

    sni->category = NULL;
    sni->id = NULL;
    sni->title = NULL;
    sni->status = NULL;
    sni->icon_name = NULL;
    sni->icon_pixmap = NULL;
    sni->attention_icon_name = NULL;
    sni->attention_icon_pixmap = NULL;
    sni->overlay_icon_name = NULL;
    sni->overlay_icon_pixmap = NULL;

    // Ignored: WindowId, AttentionMovieName, ToolTip
    //
    sni_get_property_async(sni, "Category", "s", &sni->category);
    sni_get_property_async(sni, "Id", "s", &sni->id);
    sni_get_property_async(sni, "Title", "s", &sni->title);
    sni_get_property_async(sni, "Status", "s", &sni->status);

    sni_get_property_async(sni, "IconName", "s", &sni->icon_name);
    sni_get_property_async(sni, "IconPixmap", NULL, &sni->icon_pixmap);

    sni_get_property_async(sni, "AttentionIconName", "s", &sni->attention_icon_name);
    sni_get_property_async(sni, "AttentionIconPixmap", NULL, &sni->attention_icon_pixmap);

    sni_get_property_async(sni, "OverlayIconName", "s", &sni->overlay_icon_name);
    sni_get_property_async(sni, "OverlayIconPixmap", NULL, &sni->overlay_icon_pixmap);

    sni_get_property_async(sni, "ItemIsMenu", "b", &sni->item_is_menu);
    sni_get_property_async(sni, "Menu", "o", &sni->menu);

    sni_match_signal_async(sni, "NewTitle", handle_new_title);
    sni_match_signal_async(sni, "NewIcon", handle_new_icon);
    sni_match_signal_async(sni, "NewAttentionIcon", handle_new_attention_icon);
    sni_match_signal_async(sni, "NewOverlayIcon", handle_new_overlay_icon);
    sni_match_signal_async(sni, "NewStatus", handle_new_status);

    return sni;
}

void
destroy_slot(struct sni_slot *slot) {
    sd_bus_slot_unref(slot->slot);
    free(slot);
}

void
destroy_sni(struct sni *sni)
{
    if (!sni) {
        return;
    }

    LOG_DBG("Dsretroying sni");

    // TODO: cairo_surface_destroy(sni->icon);
    free(sni->watcher_id);
    free(sni->service);
    free(sni->path);
    free(sni->status);
    free(sni->icon_name);
    if (sni->icon_pixmap)
        icon_pixmaps_dec(sni->icon_pixmap);
    free(sni->attention_icon_name);
    if (sni->attention_icon_pixmap)
        icon_pixmaps_dec(sni->attention_icon_pixmap);
    free(sni->overlay_icon_name);
    if (sni->overlay_icon_pixmap)
        icon_pixmaps_dec(sni->overlay_icon_pixmap);
    free(sni->menu);
    free(sni->icon_theme_path);

    tll_free_and_free(sni->slots, destroy_slot);
}

// Watcher -------------

static bool
using_standard_protocol(struct watcher *watcher)
{
    return watcher->interface[strlen("org.")] == 'f'; // freedesktop
}

static int
handle_lost_service(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    LOG_DBG("[Handle] lost service");
    char *service, *old_owner, *new_owner;
    int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
    if (ret < 0) {
        LOG_ERR("Failed to parse owner change message: %s", strerror(-ret));
        return ret;
    }

    if (!*new_owner) {
        struct watcher *watcher = data;
        tll_foreach(watcher->items, it)
        {
            int cmp_res = using_standard_protocol(watcher) ? strcmp(it->item, service)
                                                           : strncmp(it->item, service, strlen(service));
            if (cmp_res == 0) {
                LOG_DBG("Unregistering Status Notifier Item '%s'", it->item);
                sd_bus_emit_signal(watcher->bus, watcher_path, watcher->interface, "StatusNotifierItemUnregistered", "s",
                                   it->item);
                tll_remove_and_free(watcher->items, it, free);
                if (using_standard_protocol(watcher)) {
                    break;
                }
            }
        }

        tll_foreach(watcher->hosts, it)
        {
            LOG_DBG("item %s and service %s", it->item, service);
            if (strcmp(it->item, service) == 0) {
                LOG_DBG("Unregistering Status Notifier Host '%s'", service);
                tll_remove_and_free(watcher->hosts, it, free);
            }
        }
    }

    return 0;
}

static int
register_sni(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    char *service_or_path, *id;
    int ret = sd_bus_message_read(msg, "s", &service_or_path);
    if (ret < 0) {
        LOG_ERR("Failed to parse register SNI message: %s", strerror(-ret));
        return ret;
    }
    LOG_DBG("[callback] Register sni, service or path: %s", service_or_path);

    struct watcher *watcher = data;
    if (using_standard_protocol(watcher)) {
        id = strdup(service_or_path);
    } else {
        const char *service, *path;
        if (service_or_path[0] == '/') {
            service = sd_bus_message_get_sender(msg);
            path = service_or_path;
        } else {
            service = service_or_path;
            path = item_path;
        }
        id = format_str("%s%s", service, path);
    }

    if (!id) {
        LOG_ERR("Failed to format id");
        return ENOMEM;
    }

    bool found = false;
    tll_foreach(watcher->items, it)
    {
        if (strcmp(it->item, id) == 0) {
            found = true;
            break;
        }
    }
    if (found) {
        LOG_DBG("[Status Notifier Item] Already registered: %s", id);
        free(id);
    } else {
        LOG_DBG("[Status Notifier Item] Registering: '%s'", id);
        tll_push_back(watcher->items, id);
        sd_bus_emit_signal(watcher->bus, watcher_path, watcher->interface, "StatusNotifierItemRegistered", "s", id);
    }

    return sd_bus_reply_method_return(msg, "");
}

static int
register_host(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    LOG_DBG("[Callback] register host");
    char *service;
    int ret = sd_bus_message_read(msg, "s", &service);
    if (ret < 0) {
        LOG_ERR("Failed to parse register host message: %s", strerror(-ret));
        return ret;
    }

    struct watcher *watcher = data;
    bool found = false;
    tll_foreach(watcher->hosts, it)
    {
        if (strcmp(it->item, service) == 0) {
            found = true;
            break;
        }
    }
    if (found) {
        LOG_DBG("[Status Notifier Host] Already registered: %s", service);
    } else {
        LOG_DBG("[Status Notifier Host] Registering: %s", service);

        tll_push_back(watcher->hosts, strdup(service));
        sd_bus_emit_signal(watcher->bus, watcher_path, watcher->interface, "StatusNotifierHostRegistered", "s", "");
    }

    return sd_bus_reply_method_return(msg, "");
}

static int
get_registered_snis(sd_bus *bus, const char *obj_path, const char *interface, const char *property,
                    sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct watcher *watcher = data;
    LOG_DBG("[Callback] get registered snis");
    int r = sd_bus_message_open_container(reply, 'a', "s");
    if (r < 0)
        return r;
    tll_foreach(watcher->items, it)
    {
        r = sd_bus_message_append(reply, "s", it->item);
        if (r < 0)
            return r;
    }
    return sd_bus_message_close_container(reply);
}

static int
is_host_registered(sd_bus *bus, const char *obj_path, const char *interface, const char *property,
                   sd_bus_message *reply, void *data, sd_bus_error *error)
{
    struct watcher *watcher = data;
    int val = tll_length(watcher->hosts) > 0; // dbus expects int rather than bool
    return sd_bus_message_append_basic(reply, 'b', &val);
}

static const sd_bus_vtable watcher_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", register_sni, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", register_host, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", get_registered_snis, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", is_host_registered, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
    SD_BUS_PROPERTY("ProtocolVersion", "i", NULL, offsetof(struct watcher, version), SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
    SD_BUS_SIGNAL("StatusNotifierHostRegistered", NULL, 0),
    SD_BUS_VTABLE_END
};

void
destroy_watcher(struct watcher *watcher)
{
    if (!watcher) {
        return;
    }
    tll_free_and_free(watcher->hosts, free);
    tll_free_and_free(watcher->items, free);
    free(watcher->interface);
    free(watcher);
}

struct watcher *
create_watcher(char *protocol, sd_bus *bus)
{
    int ret;
    struct watcher *watcher = calloc(1, sizeof(struct watcher));
    if (!watcher) {
        LOG_ERR("Failed to allocate watcher");
        return NULL;
    }

    watcher->interface = format_str("org.%s.StatusNotifierWatcher", protocol);
    if (!watcher->interface) {
        LOG_ERR("Failed to format interface");
        free(watcher);
        return NULL;
    }

    sd_bus_slot *signal_slot = NULL, *vtable_slot = NULL;
    ret = sd_bus_add_object_vtable(bus, &vtable_slot, watcher_path, watcher->interface, watcher_vtable, watcher);
    if (ret < 0) {
        LOG_ERR("Failed to add object vtable: %s", strerror(-ret));
        goto error;
    }

    ret = sd_bus_match_signal(bus, &signal_slot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "NameOwnerChanged", handle_lost_service, watcher);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to unregistering events: %s", strerror(-ret));
        goto error;
    }

    ret = sd_bus_request_name(bus, watcher->interface, 0);
    if (ret < 0) {
        if (-ret == EEXIST) {
            LOG_ERR("Failed to acquire service name '%s':"
                    "another tray is already running",
                    watcher->interface);
        } else {
            LOG_ERR("Failed to acquire service name '%s': %s", watcher->interface, strerror(-ret));
        }
        goto error;
    }

    sd_bus_slot_set_floating(signal_slot, 0);
    sd_bus_slot_set_floating(vtable_slot, 0);

    watcher->bus = bus;
    hosts_t tmp1 = tll_init();
    watcher->hosts = tmp1;
    items_t tmp2 = tll_init();
    watcher->items = tmp2;
    watcher->version = 0;
    LOG_DBG("[Watcher] Registered: %s", watcher->interface);
    return watcher;
error:
    sd_bus_slot_unref(signal_slot);
    sd_bus_slot_unref(vtable_slot);
    destroy_watcher(watcher);
    return NULL;
}

// Host ----------------

static void
add_sni(struct private *m, char *id)
{
    tll_foreach(m->items, it)
    {
        if (strcmp(it->item->watcher_id, id) == 0) {
            return;
        }
    }

    LOG_DBG("[Status Notifier Item] Registering: %s", id);
    struct sni *sni = create_sni(m, id);
    if (!sni) {
        LOG_ERR("[Status Notifier Item] Failed to create: %s", id);
        return;
    }

    tll_push_back(m->items, sni);
    return;
}

static int
handle_sni_registered(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    LOG_DBG("[Signal Handle]: StatusNotifierItemRegistered");

    char *id;
    int ret = sd_bus_message_read(msg, "s", &id);
    if (ret < 0) {
        LOG_ERRNO("Failed to parse register SNI message: %s", strerror(-ret));
        return ret;
    }

    struct private *m = data;
    add_sni(m, id);

    return 0;
}

static int
handle_sni_unregistered(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    LOG_DBG("[Signal Handle]: StatusNotifierItemUnregistered");

    char *id;
    int ret = sd_bus_message_read(msg, "s", &id);
    if (ret < 0) {
        LOG_ERR("Failed to parse unregister SNI message: %s", strerror(-ret));
        return ret;
    }

    struct private *m = data;
    bool refresh = false;

    tll_foreach(m->items, it)
    {
        LOG_DBG("%s and %s", it->item->watcher_id, id);
        if (strcmp(it->item->watcher_id, id) == 0) {
            LOG_DBG("Unregistering Status Notifier Item '%s'", id);
            destroy_sni(it->item);
            tll_remove(m->items, it);
            refresh = true;
        }
    }

    if (refresh && m->mod->bar) {
        m->mod->bar->refresh(m->mod->bar);
    }
    return 0;
}

static int
get_registered_snis_callback(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    LOG_DBG("[Callback]: registered snis");
    if (sd_bus_message_is_method_error(msg, NULL)) {
        const sd_bus_error *err = sd_bus_message_get_error(msg);
        LOG_ERR("Failed to get registered SNIs: %s", err->message);
        return -sd_bus_error_get_errno(err);
    }

    int ret = sd_bus_message_enter_container(msg, 'v', NULL);
    if (ret < 0) {
        LOG_ERR("Failed to read registered SNIs: %s", strerror(-ret));
        return ret;
    }

    char **ids;
    ret = sd_bus_message_read_strv(msg, &ids);
    if (ret < 0) {
        LOG_ERR("Failed to read registered SNIs: %s", strerror(-ret));
        return ret;
    }

    if (ids) {
        struct private *m = data;
        for (char **id = ids; *id; ++id) {
            add_sni(m, *id);
            free(*id);
        }
    }

    free(ids);
    return ret;
}

static bool
register_to_watcher(struct host *host)
{
    // this is called asynchronously in case the watcher is owned by this process
    //
    int ret = sd_bus_call_method_async(host->m->bus, NULL, host->watcher_interface, watcher_path, host->watcher_interface,
                                   "RegisterStatusNotifierHost", NULL, NULL, "s", host->service);
    if (ret < 0) {
        LOG_ERR("Failed to send register call: %s", strerror(-ret));
        return false;
    }

    ret = sd_bus_call_method_async(host->m->bus, NULL, host->watcher_interface, watcher_path,
                                   "org.freedesktop.DBus.Properties", "Get", get_registered_snis_callback, host->m,
                                   "ss", host->watcher_interface, "RegisteredStatusNotifierItems");
    if (ret < 0) {
        LOG_ERR("Failed to get registered SNIs: %s", strerror(-ret));
        return false;
    }

    return true;
}

static int
handle_new_watcher(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    char *service, *old_owner, *new_owner;
    int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
    if (ret < 0) {
        LOG_ERR("Failed to parse owner change message: %s", strerror(-ret));
        return ret;
    }

    if (!*old_owner) {
        struct host *host = data;
        if (strcmp(service, host->watcher_interface) == 0) {
            register_to_watcher(host);
        }
    }

    return 0;
}

void
finish_host(struct host *host)
{
    sd_bus_release_name(host->m->bus, host->service);
    free(host->service);
    free(host->watcher_interface);
}

bool
init_host(struct host *host, char *protocol, struct private *m)
{
    int ret;

    host->watcher_interface = format_str("org.%s.StatusNotifierWatcher", protocol);
    if (!host->watcher_interface) {
        LOG_ERR("Failed to format watcher interface");
        return false;
    };

    sd_bus_slot *reg_slot = NULL, *unreg_slot = NULL, *watcher_slot = NULL;
    ret = sd_bus_match_signal(m->bus, &reg_slot, host->watcher_interface, watcher_path, host->watcher_interface,
                              "StatusNotifierItemRegistered", handle_sni_registered, m);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to registering events: %s", strerror(-ret));
        goto error;
    }
    ret = sd_bus_match_signal(m->bus, &unreg_slot, host->watcher_interface, watcher_path, host->watcher_interface,
                              "StatusNotifierItemUnregistered", handle_sni_unregistered, m);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to unregistering events: %s", strerror(-ret));
        goto error;
    }

    ret = sd_bus_match_signal(m->bus, &watcher_slot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                              "org.freedesktop.DBus", "NameOwnerChanged", handle_new_watcher, host);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to new watcher events: %s", strerror(-ret));
        goto error;
    }

    pid_t pid = getpid();
    host->service = format_str("org.%s.StatusNotifierHost-%d", protocol, pid);
    if (!host->service) {
        LOG_ERR("Failed to format host service");
        goto error;
    }

    ret = sd_bus_request_name(m->bus, host->service, 0);
    LOG_DBG("[Status Notifier Host] Initializing: %s", host->service);

    if (ret < 0) {
        LOG_ERR("Failed to acquire service name: %s", strerror(-ret));
        goto error;
    }

    host->m = m;
    if (!register_to_watcher(host)) {
        goto error;
    }

    sd_bus_slot_set_floating(reg_slot, 0);
    sd_bus_slot_set_floating(unreg_slot, 0);
    sd_bus_slot_set_floating(watcher_slot, 0);

    return true;
error:
    sd_bus_slot_unref(reg_slot);
    sd_bus_slot_unref(unreg_slot);
    sd_bus_slot_unref(watcher_slot);
    finish_host(host);
    return false;
}

// Tray ---------------

static int
handle_lost_watcher(sd_bus_message *msg, void *data, sd_bus_error *error)
{
    char *service, *old_owner, *new_owner;
    int ret = sd_bus_message_read(msg, "sss", &service, &old_owner, &new_owner);
    if (ret < 0) {
        LOG_ERR("Failed to parse owner change message: %s", strerror(-ret));
        return ret;
    }

    if (!*new_owner) {
        struct private *m = data;
        // Lock the tray while we register new watchers
        //
        if (strcmp(service, "org.freedesktop.StatusNotifierWatcher") == 0) {
            destroy_watcher(m->watcher_xdg);
            m->watcher_xdg = create_watcher("freedesktop", m->bus);
        } else if (strcmp(service, "org.kde.StatusNotifierWatcher") == 0) {
            destroy_watcher(m->watcher_kde);
            m->watcher_kde = create_watcher("kde", m->bus);
        }
    }

    return 0;
}

void
set_tray_bar(struct private *tray, struct bar *bar)
{
    tray->mod->bar = bar;
}

bool
start_tray(struct private *m)
{
    LOG_DBG("Starting tray");
    if (!m) {
        return false;
    }

    sd_bus *bus;
    int ret = sd_bus_open_user(&bus);
    if (ret < 0) {
        LOG_ERR("Failed to connect to user bus: %s", strerror(-ret));
        return false;
    }

    m->bus = bus;
    m->fd = sd_bus_get_fd(m->bus);
    if (ret < 0) {
        LOG_ERR("Failed to get user bus fd: %s", strerror(-ret));
        goto err1;
    }

    m->watcher_xdg = create_watcher("freedesktop", m->bus);
    if (m->watcher_xdg == NULL) {
        LOG_ERR("Failed to start xdg watcher");
        goto err1;
    }
    m->watcher_kde = create_watcher("kde", m->bus);
    if (m->watcher_kde == NULL) {
        LOG_ERR("Failed to start kde watcher");
        goto err2;
    }

    ret = sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                              "NameOwnerChanged", handle_lost_watcher, m);
    if (ret < 0) {
        LOG_ERR("Failed to subscribe to unregistering events: %s", strerror(-ret));
        goto err3;
    }

    snis_t tmp = tll_init();
    m->items = tmp;

    if (!init_host(&m->host_xdg, "freedesktop", m)) {
        LOG_ERR("Failed to start freedesktop host");
        goto err3;
    }
    if (!init_host(&m->host_kde, "kde", m)) {
        LOG_ERR("Failed to start kde host");
        goto err4;
    }
    return true;

err4:
    finish_host(&m->host_xdg);
err3:
    destroy_watcher(m->watcher_xdg);
err2:
    destroy_watcher(m->watcher_kde);
err1:
    sd_bus_close(m->bus);
    return false;
}

void
tray_in(sd_bus *bus, mtx_t *lock)
{
    int ret;
    while (true) {
        // Lock module while proccessing a message
        //
        mtx_lock(lock);
        ret = sd_bus_process(bus, NULL);
        mtx_unlock(lock);
        if (ret <= 0)
            break;
    }
    if (ret < 0) {
        LOG_ERR("Failed to process bus: %s", strerror(-ret));
    }
}

// Handling modules

static void
destroy(struct module *mod)
{
    struct private *m = mod->private;
    if (!m) {
        goto exit;
    }
    finish_host(&m->host_xdg);
    finish_host(&m->host_kde);
    tll_free_and_free(m->items, destroy_sni);
    destroy_watcher(m->watcher_xdg);
    destroy_watcher(m->watcher_kde);
    sd_bus_flush_close_unref(m->bus);
    m->template->destroy(m->template);

exit:
    module_default_destroy(mod);
}

static const char *
description(const struct module *mod)
{
    return "tray";
}

static struct exposable *
content(struct module *mod)
{
    struct private *m = mod->private;

    // Lock the tray while we process content
    //
    mtx_lock(&mod->lock);

    // const struct bar *bar = mod->bar;
    // const char *output_bar_is_on = mod->bar->output_name(mod->bar);

    size_t num_items = tll_length(m->items);

    struct exposable *parts[num_items];

    size_t idx = 0;
    tll_foreach(m->items, it) {
        struct sni *sni = it->item;
        LOG_DBG("status: %s", sni->status);

        struct tag_set tags = {
            .tags = (struct tag *[]) {
                tag_new_string(mod, "watcher-id", sni->watcher_id),
                tag_new_string(mod, "title", sni->title),
                tag_new_string(mod, "status", sni->status),
                tag_new_string(mod, "category", sni->category),
                tag_new_string(mod, "icon-name", sni->icon_name),
                tag_new_string(mod, "attention-icon-name", sni->attention_icon_name),
                tag_new_string(mod, "overlay-icon-name", sni->overlay_icon_name),
            },
            .count = 7,
            .icon_tags = (struct icon_tag *[]) {
                icon_tag_new_pixmap(mod, "icon-pixmap", sni->icon_pixmap),
                icon_tag_new_pixmap(mod, "attention-icon-pixmap", sni->attention_icon_pixmap),
                icon_tag_new_pixmap(mod, "overlay-icon-pixmap", sni->overlay_icon_pixmap),
            },
            .icon_count = 3,
        };

        parts[idx] = m->template->instantiate(m->template, &tags);
        tag_set_destroy(&tags);
        idx++;
    }

    mtx_unlock(&mod->lock);
    LOG_DBG("exposing content");
    return dynlist_exposable_new(parts, num_items, 0, 0);
}

static int
run(struct module *mod)
{
    struct private *m = mod->private;

    if (!start_tray(m)) {
        LOG_ERR("failed to start tray");
        return 0;
    }
    mod->bar->refresh(mod->bar);

    while (true) {
        struct pollfd fds[] = {{.fd = mod->abort_fd, .events = POLLIN}, {.fd = m->fd, .events = POLLIN | POLLHUP | POLLERR}};

        if (poll(fds, sizeof(fds) / sizeof(fds[0]), -1) < 0) {
            if (errno == EINTR)
                continue;

            LOG_ERRNO("failed to poll");
            break;
        }

        if (fds[0].revents & POLLIN) {
            break;
        }

        if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
            tray_in(m->bus, &mod->lock);
        }
    }

    return 0;
}

static struct module *
tray_new(struct particle *template)
{
    struct private *m = calloc(1, sizeof(*m));
    m->template = template;

    struct module *mod = module_common_new();
    mod->private = m;
    mod->run = &run;
    mod->destroy = &destroy;
    mod->content = &content;
    mod->description = &description;
    m->mod = mod;
    return mod;
}

static struct module *
from_conf(const struct yml_node *node, struct conf_inherit inherited)
{
    const struct yml_node *c = yml_get_value(node, "content");

    return tray_new(conf_to_particle(c, inherited));
}

static bool
verify_conf(keychain_t *chain, const struct yml_node *node)
{
    static const struct attr_info attrs[] = {
        MODULE_COMMON_ATTRS,
    };

    return conf_verify_dict(chain, node, attrs);
}

const struct module_iface module_tray_iface = {.verify_conf = &verify_conf, .from_conf = &from_conf};

#if defined(CORE_PLUGINS_AS_SHARED_LIBRARIES)
extern const struct module_iface iface __attribute__((weak, alias "module_tray_iface"));
#endif
