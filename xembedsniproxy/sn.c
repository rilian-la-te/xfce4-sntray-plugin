#include "sn.h"
#include "interfaces.h"
#include "sni-enums.h"
#include "xcb-utils.h"
#include "xtestsender.h"
#include <cairo/cairo.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib.h>
#include <unistd.h>

#define _UNUSED_ __attribute__((unused))

enum
{
	PROP_0,

	PROP_ID,
	PROP_TITLE,
	PROP_CATEGORY,
	PROP_STATUS,
	PROP_CONNECTION,
	PROP_WINDOW_ID,
	PROP_ICON,
	NB_PROPS = PROP_ICON,
};

enum
{
	SIGNAL_REGISTRATION_FAILED,
	NB_SIGNALS
};

enum InjectMode
{
	INJECT_DIRECT,
	INJECT_XTEST
};

typedef struct
{
	char *id;
	SnCategory category;
	char *title;
	SnStatus status;
	bool always_use_pixbuf;
	enum InjectMode mode;
	xcb_window_t window_id;
	xcb_window_t container_id;
	xcb_connection_t *conn;

	uint tooltip_freeze;

	uint dbus_watch_id;
	gulong dbus_sid;
	uint dbus_owner_id;
	uint dbus_reg_id;
	GDBusProxy *dbus_proxy;
	GDBusConnection *dbus_conn;
	GError *dbus_err;
	bool initialized;
	bool registered;
} StatusNotifierItemPrivate;

static uint uniq_id = 0;

static GParamSpec *sn_item_props[NB_PROPS] = {
	NULL,
};
static uint sn_item_signals[NB_SIGNALS] = {
	0,
};

#define notify(sn, prop) g_object_notify_by_pspec((GObject *)sn, sn_item_props[prop])

static void sn_item_set_property(GObject *object, uint prop_id, const GValue *value,
                                 GParamSpec *pspec);
static void sn_item_get_property(GObject *object, uint prop_id, GValue *value, GParamSpec *pspec);
static void sn_item_finalize(GObject *object);

G_DEFINE_TYPE_WITH_PRIVATE(StatusNotifierItem, sn_item, G_TYPE_OBJECT)

static void sn_item_class_init(StatusNotifierItemClass *klass)
{
	GObjectClass *o_class;

	o_class                   = G_OBJECT_CLASS(klass);
	o_class->set_property     = sn_item_set_property;
	o_class->get_property     = sn_item_get_property;
	o_class->finalize         = sn_item_finalize;
	sn_item_props[PROP_ID]    = g_param_spec_string("id",
                                                     "id",
                                                     "Unique application identifier",
                                                     NULL,
                                                     G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY);
	sn_item_props[PROP_TITLE] = g_param_spec_string("title",
	                                                "title",
	                                                "Descriptive name for the item",
	                                                NULL,
	                                                G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	sn_item_props[PROP_CATEGORY] =
	    g_param_spec_enum("category",
	                      "category",
	                      "Category of the item",
	                      sn_category_get_type(),
	                      SN_CATEGORY_APPLICATION,
	                      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS);
	sn_item_props[PROP_STATUS] = g_param_spec_enum("status",
	                                               "status",
	                                               "Status of the item",
	                                               sn_status_get_type(),
	                                               SN_STATUS_PASSIVE,
	                                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS);
	sn_item_props[PROP_CONNECTION] =
	    g_param_spec_pointer("connection",
	                         "connection",
	                         "XCB connection to serve for XWwindow",
	                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
	sn_item_props[PROP_WINDOW_ID] =
	    g_param_spec_uint("window-id",
	                      "window-id",
	                      "XCB Window ID for XWindow",
	                      0,
	                      G_MAXUINT,
	                      0,
	                      G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);

	g_object_class_install_properties(o_class, NB_PROPS, sn_item_props);

	sn_item_signals[SIGNAL_REGISTRATION_FAILED] =
	    g_signal_new("registration-failed",
	                 sn_item_get_type(),
	                 G_SIGNAL_RUN_LAST,
	                 G_STRUCT_OFFSET(StatusNotifierItemClass, registration_failed),
	                 NULL,
	                 NULL,
	                 g_cclosure_marshal_VOID__BOXED,
	                 G_TYPE_NONE,
	                 1,
	                 G_TYPE_ERROR);
}

static void sn_item_init(StatusNotifierItem *sn)
{
}

static void dbus_free(StatusNotifierItem *sn)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	if (priv->dbus_watch_id > 0)
	{
		g_bus_unwatch_name(priv->dbus_watch_id);
		priv->dbus_watch_id = 0;
	}
	if (priv->dbus_sid > 0)
	{
		g_signal_handler_disconnect(priv->dbus_proxy, priv->dbus_sid);
		priv->dbus_sid = 0;
	}
	if (G_LIKELY(priv->dbus_owner_id > 0))
	{
		g_bus_unown_name(priv->dbus_owner_id);
		priv->dbus_owner_id = 0;
	}
	if (priv->dbus_proxy)
	{
		g_clear_object(&priv->dbus_proxy);
		priv->dbus_proxy = NULL;
	}
	if (priv->dbus_reg_id > 0)
	{
		g_dbus_connection_unregister_object(priv->dbus_conn, priv->dbus_reg_id);
		priv->dbus_reg_id = 0;
	}
	if (priv->dbus_conn)
	{
		g_clear_object(&priv->dbus_conn);
		priv->dbus_conn = NULL;
	}
}

static void sn_item_finalize(GObject *object)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)object;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	g_free(priv->id);
	g_free(priv->title);
	dbus_free(sn);

	G_OBJECT_CLASS(sn_item_parent_class)->finalize(object);
}

static void dbus_notify(StatusNotifierItem *sn, uint prop)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	const char *signal;

	if (!priv->registered)
		return;

	switch (prop)
	{
	case PROP_STATUS:
	{
		signal = "NewStatus";
		g_dbus_connection_emit_signal(priv->dbus_conn,
		                              NULL,
		                              ITEM_OBJECT,
		                              ITEM_INTERFACE,
		                              signal,
		                              g_variant_new("(s)",
		                                            sn_status_get_nick(priv->status)),
		                              NULL);
		return;
	}
	case PROP_TITLE:
		signal = "NewTitle";
		break;
	case PROP_ICON:
		signal = "NewIcon";
		break;
	default:
		g_return_if_reached();
	}

	g_dbus_connection_emit_signal(priv->dbus_conn,
	                              NULL,
	                              ITEM_OBJECT,
	                              ITEM_INTERFACE,
	                              signal,
	                              NULL,
	                              NULL);
}

StatusNotifierItem *status_notifier_item_new_from_xcb_window(const char *id, SnCategory category,
                                                             xcb_connection_t *conn,
                                                             xcb_window_t window)
{
	return (StatusNotifierItem *)g_object_new(sn_item_get_type(),
	                                          "id",
	                                          id,
	                                          "category",
	                                          category,
	                                          "connection",
	                                          conn,
	                                          "window-id",
	                                          window,
	                                          NULL);
}

void sn_item_set_title(StatusNotifierItem *sn, const char *title)
{
	StatusNotifierItemPrivate *priv;

	g_return_if_fail(SN_IS_ITEM(sn));
	priv = (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	g_free(priv->title);
	priv->title = g_strdup(title);

	notify(sn, PROP_TITLE);
	dbus_notify(sn, PROP_TITLE);
}

void sn_item_set_status(StatusNotifierItem *sn, SnStatus status)
{
	StatusNotifierItemPrivate *priv;

	g_return_if_fail(SN_IS_ITEM(sn));
	priv = (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	priv->status = status;

	notify(sn, PROP_STATUS);
	dbus_notify(sn, PROP_STATUS);
}

SnStatus sn_item_get_status(StatusNotifierItem *sn)
{
	g_return_val_if_fail(SN_IS_ITEM(sn), -1);
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	return priv->status;
}

void sn_item_set_window_id(StatusNotifierItem *sn, u_int32_t window_id)
{
	StatusNotifierItemPrivate *priv;

	g_return_if_fail(SN_IS_ITEM(sn));
	priv = (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	priv->window_id = window_id;

	notify(sn, PROP_WINDOW_ID);
}

void sn_item_send_click(StatusNotifierItem *sn, uint8_t mouseButton, int x, int y)
{
	// it's best not to look at this code
	// GTK doesn't like send_events and double checks the mouse position matches where the
	// window is and is top level
	// in order to solve this we move the embed container over to where the mouse is then replay
	// the event using send_event
	// if patching, test with xchat + xchat context menus

	// note x,y are not actually where the mouse is, but the plasmoid
	// ideally we should make this match the plasmoid hit area

	g_debug("sn: received click %d with x=%d,y=%d\n", mouseButton, x, y);
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	xcb_get_geometry_cookie_t cookieSize = xcb_get_geometry(priv->conn, priv->window_id);
	g_autofree xcb_get_geometry_reply_t *clientGeom =
	    xcb_get_geometry_reply(priv->conn, cookieSize, NULL);
	if (!clientGeom)
	{
		return;
	}

	xcb_query_pointer_cookie_t cookie = xcb_query_pointer(priv->conn, priv->window_id);
	g_autofree xcb_query_pointer_reply_t *pointer =
	    xcb_query_pointer_reply(priv->conn, cookie, NULL);

	/*qCDebug(SNIPROXY) << "samescreen" << pointer->same_screen << endl
	<< "root x*y" << pointer->root_x << pointer->root_y << endl
	<< "win x*y" << pointer->win_x << pointer->win_y;*/

	// move our window so the mouse is within its geometry
	uint32_t configVals[2] = { 0, 0 };
	if (mouseButton >= XCB_BUTTON_INDEX_4)
	{
		// scroll event, take pointer position
		configVals[0] = pointer->root_x;
		configVals[1] = pointer->root_y;
	}
	else
	{
		if (pointer->root_x > x + clientGeom->width)
			configVals[0] = pointer->root_x - clientGeom->width + 1;
		else
			configVals[0] = (uint32_t)(x);
		if (pointer->root_y > y + clientGeom->height)
			configVals[1] = pointer->root_y - clientGeom->height + 1;
		else
			configVals[1] = (uint32_t)(y);
	}
	xcb_configure_window(priv->conn,
	                     priv->container_id,
	                     XCB_CONFIG_WINDOW_X | XCB_CONFIG_WINDOW_Y,
	                     configVals);

	// pull window up
	const uint32_t stackAboveData[] = { XCB_STACK_MODE_ABOVE };
	xcb_configure_window(priv->conn,
	                     priv->container_id,
	                     XCB_CONFIG_WINDOW_STACK_MODE,
	                     stackAboveData);

	// mouse down
	if (priv->mode == INJECT_DIRECT)
	{
		xcb_button_press_event_t *event =
		    (xcb_button_press_event_t *)g_malloc0(sizeof(xcb_button_press_event_t));
		memset(event, 0x00, sizeof(xcb_button_press_event_t));
		event->response_type = XCB_BUTTON_PRESS;
		event->event         = priv->window_id;
		event->time          = xcb_get_timestamp_for_connection(priv->conn);
		event->same_screen   = 1;
		event->root          = xcb_get_screen_for_connection(priv->conn, 0)->root;
		event->root_x        = x;
		event->root_y        = y;
		event->event_x       = 0;
		event->event_y       = 0;
		event->child         = 0;
		event->state         = 0;
		event->detail        = mouseButton;

		xcb_send_event(priv->conn,
		               false,
		               priv->window_id,
		               XCB_EVENT_MASK_BUTTON_PRESS,
		               (char *)event);
		g_free(event);
	}
	else
	{
		sendXTestPressed(priv->conn, mouseButton);
	}

	// mouse up
	if (priv->mode == INJECT_DIRECT)
	{
		xcb_button_release_event_t *event =
		    (xcb_button_press_event_t *)g_malloc0(sizeof(xcb_button_release_event_t));
		memset(event, 0x00, sizeof(xcb_button_release_event_t));
		event->response_type = XCB_BUTTON_RELEASE;
		event->event         = priv->window_id;
		event->time          = xcb_get_timestamp_for_connection(priv->conn);
		event->same_screen   = 1;
		event->root          = xcb_get_screen_for_connection(priv->conn, 0)->root;
		event->root_x        = x;
		event->root_y        = y;
		event->event_x       = 0;
		event->event_y       = 0;
		event->child         = 0;
		event->state         = 0;
		event->detail        = mouseButton;

		xcb_send_event(priv->conn,
		               false,
		               priv->window_id,
		               XCB_EVENT_MASK_BUTTON_RELEASE,
		               (char *)event);
		g_free(event);
	}
	else
	{
		sendXTestReleased(priv->conn, mouseButton);
	}

#ifndef VISUAL_DEBUG
	const uint32_t stackBelowData[] = { XCB_STACK_MODE_BELOW };
	xcb_configure_window(priv->conn,
	                     priv->container_id,
	                     XCB_CONFIG_WINDOW_STACK_MODE,
	                     stackBelowData);
#endif
}

static void method_call(GDBusConnection *conn _UNUSED_, const char *sender _UNUSED_,
                        const char *object _UNUSED_, const char *interface _UNUSED_,
                        const char *method, GVariant *params, GDBusMethodInvocation *invocation,
                        gpointer data)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	uint signal;
	gint x, y;
	bool ret;

	if (!g_strcmp0(method, "Scroll"))
	{
		gint delta, orientation;
		char *s_orientation;

		g_variant_get(params, "(is)", &delta, &s_orientation);
		if (!g_ascii_strcasecmp(s_orientation, "vertical"))
		{
			sn_item_send_click(sn,
			                   delta > 0 ? XCB_BUTTON_INDEX_4 : XCB_BUTTON_INDEX_5,
			                   0,
			                   0);
		}
		else
		{
			sn_item_send_click(sn, delta > 0 ? 6 : 7, 0, 0);
		}
		g_free(s_orientation);
		return;
	}
	else if (!g_strcmp0(method, "ContextMenu"))
	{
		g_variant_get(params, "(ii)", &x, &y);
		sn_item_send_click(sn, XCB_BUTTON_INDEX_3, x, y);
	}
	else if (!g_strcmp0(method, "Activate"))
	{
		g_variant_get(params, "(ii)", &x, &y);
		sn_item_send_click(sn, XCB_BUTTON_INDEX_1, x, y);
	}
	else if (!g_strcmp0(method, "SecondaryActivate"))
	{
		g_variant_get(params, "(ii)", &x, &y);
		sn_item_send_click(sn, XCB_BUTTON_INDEX_2, x, y);
	}
	else
		/* should never happen */
		g_return_if_reached();
	g_dbus_method_invocation_return_value(invocation, NULL);
}

static GVariantBuilder *get_builder_for_xcb_window(StatusNotifierItem *sn)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	GVariantBuilder *builder;
	cairo_surface_t *surface;
	cairo_t *cr;
	gint width, height, stride;
	uint *data;
	// TODO: Implement this.
	return builder;
}

static void sn_item_set_property(GObject *object, uint prop_id, const GValue *value,
                                 GParamSpec *pspec)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)object;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	switch (prop_id)
	{
	case PROP_ID: /* G_PARAM_CONSTRUCT_ONLY */
		priv->id = g_value_dup_string(value);
		break;
	case PROP_TITLE:
		sn_item_set_title(sn, g_value_get_string(value));
		break;
	case PROP_CATEGORY: /* G_PARAM_CONSTRUCT_ONLY */
		priv->category = g_value_get_enum(value);
		break;
	case PROP_STATUS:
		sn_item_set_status(sn, g_value_get_enum(value));
		break;
	case PROP_WINDOW_ID:
		sn_item_set_window_id(sn, g_value_get_uint(value));
		break;

	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void sn_item_get_property(GObject *object, uint prop_id, GValue *value, GParamSpec *pspec)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)object;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	switch (prop_id)
	{
	case PROP_ID:
		g_value_set_string(value, priv->id);
		break;
	case PROP_TITLE:
		g_value_set_string(value, priv->title);
		break;
	case PROP_CATEGORY:
		g_value_set_enum(value, priv->category);
		break;
	case PROP_STATUS:
		g_value_set_enum(value, priv->status);
		break;
	case PROP_WINDOW_ID:
		g_value_set_uint(value, priv->window_id);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static GVariant *get_prop(GDBusConnection *conn _UNUSED_, const char *sender _UNUSED_,
                          const char *object _UNUSED_, const char *interface _UNUSED_,
                          const char *property, GError **error _UNUSED_, gpointer data)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	if (!g_strcmp0(property, "Id"))
		return g_variant_new("s", priv->id);
	else if (!g_strcmp0(property, "Category"))
	{
		return g_variant_new("s", sn_category_get_nick(priv->category));
	}
	else if (!g_strcmp0(property, "Title"))
		return g_variant_new("s", (priv->title) ? priv->title : "");
	else if (!g_strcmp0(property, "Status"))
		return g_variant_new("s", sn_status_get_nick(priv->status));
	else if (!g_strcmp0(property, "WindowId"))
		return g_variant_new("i", priv->window_id);
	else if (!g_strcmp0(property, "IconPixmap"))
		return g_variant_new("a(iiay)", get_builder_for_xcb_window(sn));
	else if (!g_strcmp0(property, "ItemIsMenu"))
		return g_variant_new("b", false);

	return NULL;
}

static void dbus_failed(StatusNotifierItem *sn, GError *error, bool fatal)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	dbus_free(sn);
	g_signal_emit(sn, sn_item_signals[SIGNAL_REGISTRATION_FAILED], 0, error);
	g_error_free(error);
}

static void bus_acquired(GDBusConnection *conn, const char *name _UNUSED_, gpointer data)
{
	GError *err            = NULL;
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	GDBusInterfaceVTable interface_vtable = { .method_call  = method_call,
		                                  .get_property = get_prop,
		                                  .set_property = NULL };
	GDBusNodeInfo *info;

	info              = g_dbus_node_info_new_for_xml(item_xml, NULL);
	priv->dbus_reg_id = g_dbus_connection_register_object(conn,
	                                                      ITEM_OBJECT,
	                                                      info->interfaces[0],
	                                                      &interface_vtable,
	                                                      sn,
	                                                      NULL,
	                                                      &err);
	g_dbus_node_info_unref(info);
	if (priv->dbus_reg_id == 0)
	{
		dbus_failed(sn, err, TRUE);
		return;
	}

	priv->dbus_conn = g_object_ref(conn);
}

static void register_item_cb(GObject *sce, GAsyncResult *result, gpointer data)
{
	GError *err            = NULL;
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	GVariant *variant;

	variant = g_dbus_proxy_call_finish((GDBusProxy *)sce, result, &err);
	if (!variant)
	{
		dbus_failed(sn, err, TRUE);
		return;
	}
	g_variant_unref(variant);

	//	notify(sn, PROP_STATE);
}

static void name_acquired(GDBusConnection *conn _UNUSED_, const char *name, gpointer data)
{
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	g_dbus_proxy_call(priv->dbus_proxy,
	                  "RegisterStatusNotifierItem",
	                  g_variant_new("(s)", name),
	                  G_DBUS_CALL_FLAGS_NONE,
	                  -1,
	                  NULL,
	                  register_item_cb,
	                  sn);
	g_clear_object(&priv->dbus_proxy);
	priv->dbus_proxy = NULL;
}

static void name_lost(GDBusConnection *conn, const char *name _UNUSED_, gpointer data)
{
	GError *err            = NULL;
	StatusNotifierItem *sn = (StatusNotifierItem *)data;

	if (!conn)
		g_set_error(&err,
		            SN_ERROR,
		            SN_ERROR_NO_CONNECTION,
		            "Failed to establish DBus connection");
	else
		g_set_error(&err, SN_ERROR, SN_ERROR_NO_NAME, "Failed to acquire name for item");
	dbus_failed(sn, err, TRUE);
}

static void dbus_reg_item(StatusNotifierItem *sn)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	char buf[64], *b = buf;

	if (G_UNLIKELY(
	        g_snprintf(buf, 64, "org.kde.StatusNotifierItem-%u-%u", getpid(), ++uniq_id) >= 64))
		b = g_strdup_printf("org.kde.StatusNotifierItem-%u-%u", getpid(), uniq_id);
	priv->dbus_owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
	                                     b,
	                                     G_BUS_NAME_OWNER_FLAGS_NONE,
	                                     bus_acquired,
	                                     name_acquired,
	                                     name_lost,
	                                     sn,
	                                     NULL);
	if (G_UNLIKELY(b != buf))
		g_free(b);
}

static void watcher_signal(GDBusProxy *proxy _UNUSED_, const char *sender _UNUSED_,
                           const char *signal, GVariant *params _UNUSED_, StatusNotifierItem *sn)
{
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	if (!g_strcmp0(signal, "StatusNotifierHostRegistered"))
	{
		g_signal_handler_disconnect(priv->dbus_proxy, priv->dbus_sid);
		priv->dbus_sid = 0;

		dbus_reg_item(sn);
	}
}

static void proxy_cb(GObject *sce _UNUSED_, GAsyncResult *result, gpointer data)
{
	GError *err            = NULL;
	StatusNotifierItem *sn = (StatusNotifierItem *)data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	GVariant *variant;

	priv->dbus_proxy = g_dbus_proxy_new_for_bus_finish(result, &err);
	if (!priv->dbus_proxy)
	{
		dbus_failed(sn, err, TRUE);
		return;
	}

	variant =
	    g_dbus_proxy_get_cached_property(priv->dbus_proxy, "IsStatusNotifierHostRegistered");
	if (!variant || !g_variant_get_boolean(variant))
	{
		GDBusProxy *proxy;

		g_set_error(&err, SN_ERROR, SN_ERROR_NO_HOST, "No Host registered on the Watcher");
		if (variant)
			g_variant_unref(variant);

		/* keep the proxy, we'll wait for the signal when a host registers */
		proxy = priv->dbus_proxy;
		/* (so dbus_free() from dbus_failed() doesn't unref) */
		priv->dbus_proxy = NULL;
		dbus_failed(sn, err, FALSE);
		priv->dbus_proxy = proxy;

		priv->dbus_sid =
		    g_signal_connect(priv->dbus_proxy, "g-signal", (GCallback)watcher_signal, sn);
		return;
	}
	g_variant_unref(variant);

	dbus_reg_item(sn);
}

static void watcher_appeared(GDBusConnection *conn _UNUSED_, const char *name _UNUSED_,
                             const char *owner _UNUSED_, gpointer data)
{
	StatusNotifierItem *sn = data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	GDBusNodeInfo *info;

	g_bus_unwatch_name(priv->dbus_watch_id);
	priv->dbus_watch_id = 0;

	info = g_dbus_node_info_new_for_xml(watcher_xml, NULL);
	g_dbus_proxy_new_for_bus(G_BUS_TYPE_SESSION,
	                         G_DBUS_PROXY_FLAGS_NONE,
	                         info->interfaces[0],
	                         WATCHER_NAME,
	                         WATCHER_OBJECT,
	                         WATCHER_INTERFACE,
	                         NULL,
	                         proxy_cb,
	                         sn);
	g_dbus_node_info_unref(info);
}

static void watcher_vanished(GDBusConnection *conn _UNUSED_, const char *name _UNUSED_,
                             gpointer data)
{
	GError *err            = NULL;
	StatusNotifierItem *sn = data;
	StatusNotifierItemPrivate *priv =
	    (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);
	uint id;

	/* keep the watch active, so if a watcher shows up we'll resume the
	 * registering automatically */
	id = priv->dbus_watch_id;
	/* (so dbus_free() from dbus_failed() doesn't unwatch) */
	priv->dbus_watch_id = 0;

	g_set_error(&err, SN_ERROR, SN_ERROR_NO_WATCHER, "No Watcher found");
	dbus_failed(sn, err, FALSE);

	priv->dbus_watch_id = id;
}

void sn_item_register(StatusNotifierItem *sn)

{
	StatusNotifierItemPrivate *priv;

	g_return_if_fail(SN_IS_ITEM(sn));
	priv = (StatusNotifierItemPrivate *)sn_item_get_instance_private(sn);

	if (priv->registered)
		return;

	priv->dbus_watch_id = g_bus_watch_name(G_BUS_TYPE_SESSION,
	                                       WATCHER_NAME,
	                                       G_BUS_NAME_WATCHER_FLAGS_AUTO_START,
	                                       watcher_appeared,
	                                       watcher_vanished,
	                                       sn,
	                                       NULL);
}
