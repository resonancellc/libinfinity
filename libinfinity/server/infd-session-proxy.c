/* libinfinity - a GObject-based infinote implementation
 * Copyright (C) 2007-2015 Armin Burgmeier <armin@arbur.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 */

/**
 * SECTION:infd-session-proxy
 * @title: InfdSessionProxy
 * @short_description: Server-side session management
 * @include: libinfinity/server/infd-session-proxy.h
 * @see_also: #InfSession
 * @stability: Unstable
 *
 * #InfdSessionProxy is a manager for #InfSession that is specific for the
 * server side. The #InfdSessionProxy manages exactly one #InfSession. It is
 * usually created by #InfdDirectory when the content of a node is requested.
 *
 * #InfdSessionProxy allows to perform server-specific operations for the
 * session it manages, such as joining a local user, or removing a connection
 * from the list of subscribed connections. In addition it handles requests
 * made by its counterpart, #InfcSessionProxy, for the client side, such as
 * remote user joins.
 *
 * #InfdSessionProxy implements the #InfSessionProxy interface, which can be
 * used to access the underlying #InfSession or to join a local user.
 */

#include <libinfinity/server/infd-session-proxy.h>
#include <libinfinity/server/infd-request.h>
#include <libinfinity/common/inf-session-proxy.h>
#include <libinfinity/common/inf-request-result.h>
#include <libinfinity/common/inf-io.h>
#include <libinfinity/common/inf-xml-util.h>
#include <libinfinity/common/inf-error.h>
#include <libinfinity/inf-i18n.h>
#include <libinfinity/inf-signals.h>

#include <string.h>

typedef struct _InfdSessionProxySubscription InfdSessionProxySubscription;
struct _InfdSessionProxySubscription {
  InfXmlConnection* connection;
  guint seq_id;

  GSList* users; /* Available users joined via this connection */
};

typedef struct _InfdSessionProxyPrivate InfdSessionProxyPrivate;
struct _InfdSessionProxyPrivate {
  InfIo* io;
  InfSession* session;
  InfCommunicationHostedGroup* subscription_group;

  GSList* subscriptions;
  guint user_id_counter;

  /* Local users that do not belong to a particular connection */
  GSList* local_users;
  /* Whether there are any subscriptions / synchronizations */
  gboolean idle;
};

enum {
  PROP_0,

  /* construct/only */
  PROP_IO,
  PROP_SESSION,
  PROP_SUBSCRIPTION_GROUP,

  /* read/only */
  PROP_IDLE
};

enum {
  ADD_SUBSCRIPTION,
  REMOVE_SUBSCRIPTION,
  REJECT_USER_JOIN,

  LAST_SIGNAL
};

#define INFD_SESSION_PROXY_PRIVATE(obj) (G_TYPE_INSTANCE_GET_PRIVATE((obj), INFD_TYPE_SESSION_PROXY, InfdSessionProxyPrivate))

static guint session_proxy_signals[LAST_SIGNAL];

static void infd_session_proxy_communication_object_iface_init(InfCommunicationObjectInterface* iface);
static void infd_session_proxy_session_proxy_iface_init(InfSessionProxyInterface* iface);
G_DEFINE_TYPE_WITH_CODE(InfdSessionProxy, infd_session_proxy, G_TYPE_OBJECT,
  G_ADD_PRIVATE(InfdSessionProxy)
  G_IMPLEMENT_INTERFACE(INF_COMMUNICATION_TYPE_OBJECT, infd_session_proxy_communication_object_iface_init)
  G_IMPLEMENT_INTERFACE(INF_TYPE_SESSION_PROXY, infd_session_proxy_session_proxy_iface_init))

/*
 * SessionProxy subscriptions.
 */

static InfdSessionProxySubscription*
infd_session_proxy_subscription_new(InfXmlConnection* connection,
                                    guint seq_id)
{
  InfdSessionProxySubscription* subscription;
  subscription = g_slice_new(InfdSessionProxySubscription);

  subscription->connection = connection;
  subscription->seq_id = seq_id;
  subscription->users = NULL;

  g_object_ref(G_OBJECT(connection));
  return subscription;
}

static void
infd_session_proxy_subscription_free(InfdSessionProxySubscription* subscr)
{
  g_object_unref(G_OBJECT(subscr->connection));
  g_slist_free(subscr->users);
  g_slice_free(InfdSessionProxySubscription, subscr);
}

static GSList*
infd_session_proxy_find_subscription_item(InfdSessionProxy* proxy,
                                          InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;
  GSList* item;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  for(item = priv->subscriptions; item != NULL; item = g_slist_next(item))
    if( ((InfdSessionProxySubscription*)item->data)->connection == connection)
      return item;

  return NULL;
}

static InfdSessionProxySubscription*
infd_session_proxy_find_subscription(InfdSessionProxy* proxy,
                                     InfXmlConnection* connection)
{
  GSList* item;

  item = infd_session_proxy_find_subscription_item(proxy, connection);
  if(item == NULL) return NULL;

  return (InfdSessionProxySubscription*)item->data;
}

static gboolean
infd_session_proxy_check_idle(InfdSessionProxy* proxy)
{
  InfdSessionProxyPrivate* priv;
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  if(priv->subscriptions == NULL &&
     priv->local_users == NULL &&
     !inf_session_has_synchronizations(priv->session))
  {
    return TRUE;
  }

  return FALSE;
}

static void
infd_session_proxy_user_notify_status_cb(InfUser* user,
                                         const GParamSpec* pspec,
                                         gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscr;

  if(inf_user_get_status(user) == INF_USER_UNAVAILABLE)
  {
    proxy = INFD_SESSION_PROXY(user_data);
    priv = INFD_SESSION_PROXY_PRIVATE(proxy);

    if(inf_user_get_connection(user))
    {
      subscr = infd_session_proxy_find_subscription(
        proxy,
        inf_user_get_connection(user)
      );

      g_assert(subscr != NULL);
      subscr->users = g_slist_remove(subscr->users, user);

      g_object_set(G_OBJECT(user), "connection", NULL, NULL);
    }
    else
    {
      priv->local_users = g_slist_remove(priv->local_users, user);

      if(priv->idle == FALSE && infd_session_proxy_check_idle(proxy) == TRUE)
      {
        priv->idle = TRUE;
        g_object_notify(G_OBJECT(proxy), "idle");
      }
    }

    inf_signal_handlers_disconnect_by_func(
      G_OBJECT(user),
      G_CALLBACK(infd_session_proxy_user_notify_status_cb),
      proxy
    );
  }
}

/*
 * Utility functions.
 */

static gboolean
infd_session_proxy_make_seq(InfdSessionProxy* proxy,
                            InfXmlConnection* connection,
                            xmlNodePtr xml,
                            gchar** seq,
                            GError** error)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  GError* local_error;
  guint seq_num;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  local_error = NULL;
  if(!inf_xml_util_get_attribute_uint(xml, "seq", &seq_num, &local_error))
  {
    if(local_error)
    {
      g_propagate_error(error, local_error);
      return FALSE;
    }

    *seq = NULL;
    return TRUE;
  }

  subscription = infd_session_proxy_find_subscription(proxy, connection);
  g_assert(subscription != NULL);

  *seq = g_strdup_printf("%u/%u", subscription->seq_id, seq_num);
  return TRUE;
}

/* Performs a user join on the given proxy. If connection is not null, the
 * user join is made from that connection, otherwise a local user join is
 * performed. seq is the seq of the user join request and used in
 * the reply, or NULL if there was no seq. */
static InfUser*
infd_session_proxy_perform_user_join(InfdSessionProxy* proxy,
                                     InfXmlConnection* connection,
                                     const gchar* seq,
                                     GArray* user_props,
                                     GError** error)
{
  InfSessionClass* session_class;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  InfUser* user;
  const GParameter* name_param;
  GParameter* param;
  gboolean result;
  xmlNodePtr xml;
  guint i;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  session_class = INF_SESSION_GET_CLASS(priv->session);

  g_assert(session_class->validate_user_props != NULL);
  g_assert(session_class->user_new != NULL);

  name_param = inf_session_lookup_user_property(
    (const GParameter*)user_props->data,
    user_props->len,
    "name"
  );

  if(name_param == NULL)
  {
    g_set_error_literal(
      error,
      inf_request_error_quark(),
      INF_REQUEST_ERROR_NO_SUCH_ATTRIBUTE,
      _("Request does not contain required attribute \"name\"")
    );

    return NULL;
  }

  /* TODO: Isn't this already done in validate_user_props? */
  user = inf_user_table_lookup_user_by_name(
    inf_session_get_user_table(priv->session),
    g_value_get_string(&name_param->value)
  );

  if(user != NULL && inf_user_get_status(user) != INF_USER_UNAVAILABLE)
  {
    g_set_error(
      error,
      inf_user_error_quark(),
      INF_USER_ERROR_NAME_IN_USE,
      _("Name \"%s\" already in use"),
      g_value_get_string(&name_param->value)
    );

    return NULL;
  }

  /* User join requests must not have the id value set because the server
   * chooses an ID, or reuses an existing one in the case of a rejoin. */
  param = inf_session_get_user_property(user_props, "id");
  if(G_IS_VALUE(&param->value))
  {
    g_set_error_literal(
      error,
      inf_request_error_quark(),
      INF_REQUEST_ERROR_INVALID_ATTRIBUTE,
      inf_user_strerror(INF_USER_ERROR_ID_PROVIDED)
    );

    return NULL;
  }

  /* The user ID counter is increased in the add-user default signal
   * handler. */
  g_value_init(&param->value, G_TYPE_UINT);

  /* Reuse user ID on rejoin. */
  if(user != NULL)
    g_value_set_uint(&param->value, inf_user_get_id(user));
  else
    g_value_set_uint(&param->value, priv->user_id_counter);

  /* Check user status. It must not be unavailable on join/rejoin */
  param = inf_session_get_user_property(user_props, "status");
  if(G_IS_VALUE(&param->value))
  {
    if(g_value_get_enum(&param->value) == INF_USER_UNAVAILABLE)
    {
      g_set_error_literal(
        error,
        inf_request_error_quark(),
        INF_REQUEST_ERROR_INVALID_ATTRIBUTE,
        _("\"status\" attribute is \"unavailable\" in user join request")
      );

      return NULL;
    }
  }
  else
  {
    g_value_init(&param->value, INF_TYPE_USER_STATUS);
    g_value_set_enum(&param->value, INF_USER_ACTIVE);
  }

  /* flags should not be set by get_xml_user_props, nor given
   * to infd_session_proxy_add_user. */
  param = inf_session_get_user_property(user_props, "flags");
  g_assert(!G_IS_VALUE(&param->value));

  g_value_init(&param->value, INF_TYPE_USER_FLAGS);
  if(connection == NULL)
    g_value_set_flags(&param->value, INF_USER_LOCAL);
  else
    g_value_set_flags(&param->value, 0);


  /* same with connection */
  param = inf_session_get_user_property(user_props, "connection");
  g_assert(!G_IS_VALUE(&param->value));
  g_value_init(&param->value, INF_TYPE_XML_CONNECTION);
  g_value_set_object(&param->value, G_OBJECT(connection));

  /* Validate properties, but exclude the rejoining user from the check.
   * Otherwise, we would get conflicts because the name and the ID
   * of the request and the rejoining user are the same. */
  result = session_class->validate_user_props(
    priv->session,
    (const GParameter*)user_props->data,
    user_props->len,
    user,
    error
  );

  if(result == FALSE)
    return NULL;

  g_signal_emit(
    proxy,
    session_proxy_signals[REJECT_USER_JOIN],
    0,
    connection,
    user_props,
    user,
    &result
  );

  if(result == TRUE)
  {
    g_set_error_literal(
      error,
      inf_request_error_quark(),
      INF_REQUEST_ERROR_NOT_AUTHORIZED,
      _("Permission denied")
    );

    return NULL;
  }

  if(user == NULL)
  {
    user = inf_session_add_user(
      priv->session,
      (const GParameter*)user_props->data,
      user_props->len
    );

    g_assert(user != NULL);
    xml = xmlNewNode(NULL, (const xmlChar*)"user-join");
  }
  else
  {
    g_object_freeze_notify(G_OBJECT(user));

    /* Set properties on already existing user object. */
    for(i = 0; i < user_props->len; ++ i)
    {
      param = &g_array_index(user_props, GParameter, i);

      /* Don't set name and ID because they did not change, and we are not
       * even allowed to set ID because it is construct only. */
      if(strcmp(param->name, "name") != 0 && strcmp(param->name, "id") != 0)
        g_object_set_property(G_OBJECT(user), param->name, &param->value);
    }

    g_object_thaw_notify(G_OBJECT(user));

    xml = xmlNewNode(NULL, (const xmlChar*)"user-rejoin");
  }

  inf_session_user_to_xml(priv->session, user, xml);
  if(seq != NULL) inf_xml_util_set_attribute(xml, "seq", seq);

  /* TODO: Send with "connection" to subscriptions that are in the same
   * network, and that are non-local. */

  g_signal_connect(
    G_OBJECT(user),
    "notify::status",
    G_CALLBACK(infd_session_proxy_user_notify_status_cb),
    proxy
  );

  inf_session_send_to_subscriptions(priv->session, xml);

  if(connection != NULL)
  {
    subscription = infd_session_proxy_find_subscription(proxy, connection);
    g_assert(subscription != NULL);

    subscription->users = g_slist_prepend(subscription->users, user);
  }
  else
  {
    priv->local_users = g_slist_prepend(priv->local_users, user);

    if(priv->idle == TRUE)
    {
      priv->idle = FALSE;
      g_object_notify(G_OBJECT(proxy), "idle");
    }
  }

  return user;
}

/*
 * Signal handlers.
 */

static void
infd_session_proxy_member_removed_cb(InfCommunicationGroup* group,
                                     InfXmlConnection* connection,
                                     gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  xmlNodePtr xml;
  GSList* item;
  InfUser* user;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  subscription = infd_session_proxy_find_subscription(proxy, connection);
  g_assert(subscription != NULL);

  /* TODO: Only send user-status-change to users that don't have a direct
   * connection to the closed connection. */
  for(item = subscription->users; item != NULL; item = g_slist_next(item))
  {
    user = INF_USER(item->data);

    /* Send user-status-change to remaining subscriptions. */
    /* Note: We cannot simply use inf_session_set_user_status because it
     * would also try to send the status change to the subscription we are
     * removing, and because it only works for local users. */
    xml = xmlNewNode(NULL, (const xmlChar*)"user-status-change");
    inf_xml_util_set_attribute_uint(xml, "id", inf_user_get_id(user));

    inf_xml_util_set_attribute(
      xml,
      "status",
      inf_user_status_to_string(INF_USER_UNAVAILABLE)
    );

    /* The actual status change is performed in the default signal handler
     * of the remove-subscription signal. */
    inf_session_send_to_subscriptions(priv->session, xml);
  }

  g_signal_emit(
    proxy,
    session_proxy_signals[REMOVE_SUBSCRIPTION],
    0,
    connection
  );
}

static void
infd_session_proxy_add_user_cb(InfUserTable* user_table,
                               InfUser* user,
                               gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfXmlConnection* sync_conn;
  InfdSessionProxySubscription* subscription;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* Make sure that we generate a non-existing user ID for the next user. */
  if(priv->user_id_counter <= inf_user_get_id(user))
    priv->user_id_counter = inf_user_get_id(user) + 1;

  if(inf_session_get_status(priv->session) == INF_SESSION_SYNCHRONIZING)
  {
    if(inf_user_get_status(user) != INF_USER_UNAVAILABLE)
    {
      g_object_get(
        G_OBJECT(priv->session),
        "sync-connection",
        &sync_conn,
        NULL
      );

      g_assert(sync_conn != NULL);
      subscription = infd_session_proxy_find_subscription(proxy, sync_conn);

      /* During synchronization, available users are always considered to
       * belong to the synchronizing connection. Everything else is just not
       * supported and causes session closure. */
      if(sync_conn != inf_user_get_connection(user) || subscription == NULL)
      {
        /* This actually cancels the synchronization: */
        inf_session_close(priv->session);
      }
      else
      {
        subscription->users = g_slist_prepend(subscription->users, user);

        g_signal_connect(
          G_OBJECT(user),
          "notify::status",
          G_CALLBACK(infd_session_proxy_user_notify_status_cb),
          proxy
        );
      }

      g_object_unref(sync_conn);
    }
  }
}

static void
infd_session_proxy_synchronization_begin_cb(InfSession* session,
                                            InfCommunicationGroup* group,
                                            InfXmlConnection* connection,
                                            gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  if(priv->idle)
  {
    priv->idle = FALSE;
    g_object_notify(G_OBJECT(proxy), "idle");
  }
}

static void
infd_session_proxy_synchronization_complete_cb(InfSession* session,
                                               InfXmlConnection* conn,
                                               gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* Set idle if no more synchronizations are running */
  if(!priv->idle && priv->subscriptions == NULL &&
     priv->local_users == NULL &&
     !inf_session_has_synchronizations(session))
  {
    priv->idle = TRUE;
    g_object_notify(G_OBJECT(proxy), "idle");
  }
}

static void
infd_session_proxy_synchronization_failed_cb_before(InfSession* session,
                                                    InfXmlConnection* conn,
                                                    const GError* error,
                                                    gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfSessionStatus status;
  InfdSessionProxySubscription* subscription;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  g_object_get(session, "status", &status, NULL);

  /* We do not need handle the status == INF_SESSION_PROXY_SYNCHRONIZING case
   * since there cannot be any subscriptions while we are synchronizing. */

  if(status == INF_SESSION_RUNNING)
  {
    /* Remove from subscription group if the connection was subscribed */
    subscription = infd_session_proxy_find_subscription(proxy, conn);
    if(subscription != NULL)
    {
      inf_communication_hosted_group_remove_member(
        priv->subscription_group,
        conn
      );
    }
  }
}

static void
infd_session_proxy_synchronization_failed_cb_after(InfSession* session,
                                                   InfXmlConnection* conn,
                                                   const GError* error,
                                                   gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* Set idle if no more synchronizations are running */
  if(!priv->idle && priv->subscriptions == NULL &&
     !inf_session_has_synchronizations(session))
  {
    priv->idle = TRUE;
    g_object_notify(G_OBJECT(proxy), "idle");
  }
}

static void
infd_session_proxy_session_close_cb(InfSession* session,
                                    gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->subscription_group),
    G_CALLBACK(infd_session_proxy_member_removed_cb),
    proxy
  );

  while(priv->subscriptions != NULL)
  {
    subscription = (InfdSessionProxySubscription*)priv->subscriptions->data;

    /* Note that this does not call our signal handler because we already
     * disconnected it. This way, we make sure not to send user status updates
     * which would be pointless since we are closing the group anyway. */
    infd_session_proxy_unsubscribe(proxy, subscription->connection);

    /* However, this means we need to emit the unsubscribe signal ourselves */
    g_signal_emit(
      proxy,
      session_proxy_signals[REMOVE_SUBSCRIPTION],
      0,
      subscription->connection
    );
  }

  /* Set local users to unavailable */
  while(priv->local_users != NULL)
  {
    g_object_set(
      G_OBJECT(priv->local_users->data),
      "status", INF_USER_UNAVAILABLE,
      NULL
    );
  }

  g_object_unref(priv->subscription_group);
  priv->subscription_group = NULL;
}

/*
 * GObject overrides.
 */

static void
infd_session_proxy_init(InfdSessionProxy* session_proxy)
{
  InfdSessionProxyPrivate* priv;
  priv = INFD_SESSION_PROXY_PRIVATE(session_proxy);

  priv->io = NULL;
  priv->subscriptions = NULL;
  priv->subscription_group = NULL;
  priv->user_id_counter = 1;
  priv->local_users = NULL;
  priv->idle = TRUE;
}

static void
infd_session_proxy_constructed(GObject* object)
{
  InfdSessionProxy* session_proxy;
  InfdSessionProxyPrivate* priv;

  session_proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(session_proxy);

  G_OBJECT_CLASS(infd_session_proxy_parent_class)->constructed(object);

  g_assert(priv->subscription_group != NULL);
  g_assert(priv->session != NULL);

  /* Set unidle when session is currently being synchronized */
  if(inf_session_get_status(priv->session) == INF_SESSION_SYNCHRONIZING ||
     priv->local_users != NULL)
  {
    priv->idle = FALSE;
  }

  /* TODO: We could perhaps optimize by only setting the subscription
   * group when there are subscribed connections. */
  inf_session_set_subscription_group(
    priv->session,
    INF_COMMUNICATION_GROUP(priv->subscription_group)
  );
}

static void
infd_session_proxy_dispose(GObject* object)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfCommunicationManager* manager;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  manager = inf_session_get_communication_manager(priv->session);
  g_object_ref(manager);

  g_slist_free(priv->local_users);
  priv->local_users = NULL;

  /* We need to close the session explicitely before we unref so that
   * the signal handler for the close signal is called. */
  /* Note this emits the close signal, removing all subscriptions and
   * the subscription group */
  if(inf_session_get_status(priv->session) != INF_SESSION_CLOSED)
    inf_session_close(priv->session);

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_session_close_cb),
    proxy
  );

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(inf_session_get_user_table(priv->session)),
    G_CALLBACK(infd_session_proxy_add_user_cb),
    proxy
  );

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_synchronization_begin_cb),
    proxy
  );

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_synchronization_complete_cb),
    proxy
  );

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_synchronization_failed_cb_before),
    proxy
  );

  inf_signal_handlers_disconnect_by_func(
    G_OBJECT(priv->session),
    G_CALLBACK(infd_session_proxy_synchronization_failed_cb_after),
    proxy
  );

  g_object_unref(priv->session);
  priv->session = NULL;

  g_assert(priv->subscription_group == NULL);
  g_assert(priv->subscriptions == NULL);

  g_object_unref(priv->io);
  priv->io = NULL;

  g_object_unref(manager);

  G_OBJECT_CLASS(infd_session_proxy_parent_class)->dispose(object);
}

static void
infd_session_proxy_session_init_user_func(InfUser* user,
                                          gpointer user_data)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(user_data);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  if(priv->user_id_counter <= inf_user_get_id(user))
    priv->user_id_counter = inf_user_get_id(user) + 1;
}

static void
infd_session_proxy_set_property(GObject* object,
                                guint prop_id,
                                const GValue* value,
                                GParamSpec* pspec)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  switch(prop_id)
  {
  case PROP_IO:
    g_assert(priv->io == NULL); /* construct only */
    priv->io = INF_IO(g_value_dup_object(value));
    break;
  case PROP_SESSION:
    g_assert(priv->session == NULL); /* construct only */
    priv->session = INF_SESSION(g_value_dup_object(value));

    /* Adjust user id counter so the next joining user gets a free ID */
    /* TODO: Add local users to priv->local_users, assert that there are no
     * available non-local users. */
    inf_user_table_foreach_user(
      inf_session_get_user_table(priv->session),
      infd_session_proxy_session_init_user_func,
      proxy
    );

    /* This in not connect_after to fix bug #499. This makes more sense
     * anyway, because otherwise the signal handler does not have any
     * synchronization info anymore. */
    g_signal_connect(
      G_OBJECT(priv->session),
      "close",
      G_CALLBACK(infd_session_proxy_session_close_cb),
      proxy
    );

    g_signal_connect(
      G_OBJECT(inf_session_get_user_table(priv->session)),
      "add-user",
      G_CALLBACK(infd_session_proxy_add_user_cb),
      proxy
    );

    g_signal_connect_after(
      G_OBJECT(priv->session),
      "synchronization-begin",
      G_CALLBACK(infd_session_proxy_synchronization_begin_cb),
      proxy
    );

    g_signal_connect_after(
      G_OBJECT(priv->session),
      "synchronization-complete",
      G_CALLBACK(infd_session_proxy_synchronization_complete_cb),
      proxy
    );

    g_signal_connect(
      G_OBJECT(priv->session),
      "synchronization-failed",
      G_CALLBACK(infd_session_proxy_synchronization_failed_cb_before),
      proxy
    );

    g_signal_connect_after(
      G_OBJECT(priv->session),
      "synchronization-failed",
      G_CALLBACK(infd_session_proxy_synchronization_failed_cb_after),
      proxy
    );

    break;
  case PROP_SUBSCRIPTION_GROUP:
    g_assert(priv->subscription_group == NULL); /* construct only */
    priv->subscription_group =
      INF_COMMUNICATION_HOSTED_GROUP(g_value_dup_object(value));

    g_signal_connect(
      G_OBJECT(priv->subscription_group),
      "member-removed",
      G_CALLBACK(infd_session_proxy_member_removed_cb),
      proxy
    );

    break;
  case PROP_IDLE:
    /* read/only */
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

static void
infd_session_proxy_get_property(GObject* object,
                                guint prop_id,
                                GValue* value,
                                GParamSpec* pspec)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  switch(prop_id)
  {
  case PROP_IO:
    g_value_set_object(value, priv->io);
    break;
  case PROP_SESSION:
    g_value_set_object(value, priv->session);
    break;
  case PROP_SUBSCRIPTION_GROUP:
    g_value_set_object(value, priv->subscription_group);
    break;
  case PROP_IDLE:
    g_value_set_boolean(value, priv->idle);
    break;
  default:
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
    break;
  }
}

/*
 * Default signal handlers
 */

static void
infd_session_proxy_add_subscription(InfdSessionProxy* proxy,
                                    InfXmlConnection* connection,
                                    guint seq_id)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  g_assert(infd_session_proxy_find_subscription(proxy, connection) == NULL);

  subscription = infd_session_proxy_subscription_new(connection, seq_id);
  priv->subscriptions = g_slist_prepend(priv->subscriptions, subscription);

  if(priv->idle == TRUE)
  {
    priv->idle = FALSE;
    g_object_notify(G_OBJECT(proxy), "idle");
  }
}

static void
infd_session_proxy_remove_subscription(InfdSessionProxy* proxy,
                                       InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscr;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  subscr = infd_session_proxy_find_subscription(proxy, connection);

  g_assert(subscr != NULL);

  /* TODO: Cancel synchronization if the synchronization to this subscription
   * did not yet finish. */

  while(subscr->users)
  {
    /* The signal handler of the user's notify::status signal removes the user
     * from the subscription. */
    g_object_set(
      G_OBJECT(subscr->users->data),
      "status", INF_USER_UNAVAILABLE,
      NULL
    );
  }

  priv->subscriptions = g_slist_remove(priv->subscriptions, subscr);
  infd_session_proxy_subscription_free(subscr);

  if(priv->idle == FALSE && infd_session_proxy_check_idle(proxy) == TRUE)
  {
    priv->idle = TRUE;
    g_object_notify(G_OBJECT(proxy), "idle");
  }
}

static gboolean
infd_session_proxy_reject_user_join(InfdSessionProxy* proxy,
                                    InfXmlConnection* connection,
                                    const GArray* user_properties,
                                    InfUser* user_rejoin)
{
  /* Allow user join by default */
  return FALSE;
}

/*
 * Message handling.
 */

static gboolean
infd_session_proxy_handle_user_join(InfdSessionProxy* proxy,
                                    InfXmlConnection* connection,
                                    xmlNodePtr xml,
                                    GError** error)
{
  InfdSessionProxyPrivate* priv;
  InfSessionClass* session_class;
  GArray* array;
  InfUser* user;
  gchar* seq;
  guint i;

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  session_class = INF_SESSION_GET_CLASS(priv->session);

  if(!infd_session_proxy_make_seq(proxy, connection, xml, &seq, error))
    return FALSE;

  array = session_class->get_xml_user_props(
    priv->session,
    connection,
    xml
  );

  user = infd_session_proxy_perform_user_join(
    proxy,
    connection,
    seq,
    array,
    error
  );

  for(i = 0; i < array->len; ++ i)
    g_value_unset(&g_array_index(array, GParameter, i).value);

  g_array_free(array, TRUE);
  g_free(seq);

  if(user == NULL)
    return FALSE;

  return TRUE;
}

static gboolean
infd_session_proxy_handle_session_unsubscribe(InfdSessionProxy* proxy,
                                              InfXmlConnection* connection,
                                              const xmlNodePtr xml,
                                              GError** error)
{
  InfdSessionProxyPrivate* priv;
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  g_assert(infd_session_proxy_find_subscription(proxy, connection) != NULL);

  inf_communication_hosted_group_remove_member(
    priv->subscription_group,
    connection
  );

  return TRUE;
}

/*
 * InfCommunicationObject implementation
 */

static void
infd_session_proxy_communication_object_sent(InfCommunicationObject* object,
                                             InfXmlConnection* connection,
                                             xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(object);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);

  inf_communication_object_sent(
    INF_COMMUNICATION_OBJECT(priv->session),
    connection,
    node
  );
}

static void
infd_session_proxy_communication_object_enqueued(InfCommunicationObject* obj,
                                                 InfXmlConnection* connection,
                                                 xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;

  proxy = INFD_SESSION_PROXY(obj);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);

  inf_communication_object_enqueued(
    INF_COMMUNICATION_OBJECT(priv->session),
    connection,
    node
  );
}

static InfCommunicationScope
infd_session_proxy_communication_object_received(InfCommunicationObject* obj,
                                                 InfXmlConnection* connection,
                                                 xmlNodePtr node)
{
  InfdSessionProxy* proxy;
  InfdSessionProxyPrivate* priv;
  InfSessionSyncStatus status;
  GError* local_error;
  xmlNodePtr reply_xml;
  gchar* seq;

  proxy = INFD_SESSION_PROXY(obj);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Don't forward for messages the proxy issued */

  g_assert(priv->session != NULL);
  status = inf_session_get_synchronization_status(priv->session, connection);
  local_error = NULL;

  if(status != INF_SESSION_SYNC_NONE)
  {
    return inf_communication_object_received(
      INF_COMMUNICATION_OBJECT(priv->session),
      connection,
      node
    );
  }
  else
  {
    if(strcmp((const char*)node->name, "user-join") == 0)
    {
      infd_session_proxy_handle_user_join(
        proxy,
        connection,
        node,
        &local_error
      );
    }
    else if(strcmp((const char*)node->name, "session-unsubscribe") == 0)
    {
      /* TODO: Handle this in InfSession, if possible */
      infd_session_proxy_handle_session_unsubscribe(
        proxy,
        connection,
        node,
        &local_error
      );
    }
    else
    {
      return inf_communication_object_received(
        INF_COMMUNICATION_OBJECT(priv->session),
        connection,
        node
      );
    }
  }

  if(local_error != NULL)
  {
    if(!infd_session_proxy_make_seq(proxy, connection, node, &seq, NULL))
      seq = NULL;

    /* Only send request-failed when it was a proxy-related request */
    reply_xml = inf_xml_util_new_node_from_error(local_error,
                                                 NULL,
                                                 "request-failed");

    if(seq != NULL) inf_xml_util_set_attribute(reply_xml, "seq", seq);
    g_free(seq);

    inf_communication_group_send_message(
      INF_COMMUNICATION_GROUP(priv->subscription_group),
      connection,
      reply_xml
    );

    g_error_free(local_error);
  }

  /* Don't forward proxy-related messages */
  return INF_COMMUNICATION_SCOPE_PTP;
}

/*
 * InfSessionProxy implementation
 */

static InfRequest*
infd_session_proxy_session_proxy_join_user(InfSessionProxy* proxy,
                                           guint n_params,
                                           const GParameter* params,
                                           InfRequestFunc func,
                                           gpointer user_data)
{
  InfdRequest* request;
  GArray* array;

  guint i;
  GParameter param;

  GError* error;
  InfUser* user;

  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), NULL);

  request = g_object_new(
    INFD_TYPE_REQUEST,
    "type", "user-join",
    NULL
  );

  if(func != NULL)
  {
    g_signal_connect_after(
      G_OBJECT(request),
      "finished",
      G_CALLBACK(func),
      user_data
    );
  }

  array = g_array_sized_new(
    FALSE,
    FALSE,
    sizeof(GParameter),
    n_params
  );

  g_array_append_vals(array, params, n_params);

  error = NULL;
  user = infd_session_proxy_perform_user_join(
    INFD_SESSION_PROXY(proxy),
    NULL,
    NULL,
    array,
    &error
  );

  /* Remove only those parameters that were added by the
   * infd_session_proxy_perform_user_join function */
  for(i = n_params; i < array->len; ++i)
    g_value_unset(&g_array_index(array, GParameter, i).value);
  g_array_free(array, TRUE);

  if(error != NULL)
  {
    inf_request_fail(INF_REQUEST(request), error);
  }
  else
  {
    inf_request_finish(
      INF_REQUEST(request),
      inf_request_result_make_join_user(proxy, user)
    );
  }

  if(error) g_error_free(error);
  g_object_unref(request);
  return NULL;
}

/*
 * GType registration.
 */

static void
infd_session_proxy_class_init(InfdSessionProxyClass* proxy_class)
{
  GObjectClass* object_class;
  object_class = G_OBJECT_CLASS(proxy_class);

  object_class->constructed = infd_session_proxy_constructed;
  object_class->dispose = infd_session_proxy_dispose;
  object_class->set_property = infd_session_proxy_set_property;
  object_class->get_property = infd_session_proxy_get_property;

  proxy_class->add_subscription = infd_session_proxy_add_subscription;
  proxy_class->remove_subscription = infd_session_proxy_remove_subscription;
  proxy_class->reject_user_join = infd_session_proxy_reject_user_join;

  g_object_class_install_property(
    object_class,
    PROP_IO,
    g_param_spec_object(
      "io",
      "Io",
      "The InfIo object for scheduling events",
      INF_TYPE_IO,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_SUBSCRIPTION_GROUP,
    g_param_spec_object(
      "subscription-group",
      "Subscription group",
      "The communication manager group of subscribed connections",
      INF_COMMUNICATION_TYPE_HOSTED_GROUP,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY
    )
  );

  g_object_class_install_property(
    object_class,
    PROP_IDLE,
    g_param_spec_boolean(
      "idle",
      "Idle",
      "The session is considered idle when are no subscriptions and no "
      "synchronizations",
      TRUE,
      G_PARAM_READABLE
    )
  );

  g_object_class_override_property(object_class, PROP_SESSION, "session");

  /**
   * InfdSessionProxy::add-subscription:
   * @proxy: The #InfdSessionProxy emitting the signal.
   * @connection: The subscribed #InfXmlConnection.
   * @seq_id: The sequence identifier for @connection as passed to
   * infd_session_proxy_subscribe_to().
   *
   * Emitted every time a connection is subscribed to the session.
   **/
  session_proxy_signals[ADD_SUBSCRIPTION] = g_signal_new(
    "add-subscription",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfdSessionProxyClass, add_subscription),
    NULL, NULL,
    NULL,
    G_TYPE_NONE,
    2,
    INF_TYPE_XML_CONNECTION,
    G_TYPE_UINT
  );

  /**
   * InfdSessionProxy::remove-subscription:
   * @proxy: The #InfdSessionProxy emitting the signal.
   * @connection: The unsubscribed #InfXmlConnection.
   *
   * Emitted every time a connection is unsubscribed to the session, or a
   * subscription is removed because the session is closed.
   **/
  session_proxy_signals[REMOVE_SUBSCRIPTION] = g_signal_new(
    "remove-subscription",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfdSessionProxyClass, remove_subscription),
    NULL, NULL,
    g_cclosure_marshal_VOID__OBJECT,
    G_TYPE_NONE,
    1,
    INF_TYPE_XML_CONNECTION
  );

  /**
   * InfdSessionProxy::reject-user-join:
   * @proxy: The #InfdSessionProxy emitting the signal.
   * @connection: A subscribed #InfXmlConnection requesting the user join.
   * @user_properties: An array with the properties for the new user.
   * @rejoin_user: The existing unavailable user that is being rejoined, or
   * %NULL.
   *
   * This signal is emitted before every remote user join. The signal handler
   * can return %TRUE in which case the #InfdSessionProxy does not allow the
   * user join with %INF_REQUEST_ERROR_NOT_AUTHORIZED error. If there is more
   * than one signal handler, then if one of them returns %TRUE the user
   * join is rejected.
   *
   * The @user_properties parameter is a #GArray of #GParameter values. It
   * contains the construct properties for the #InfUser object that would be
   * created if the user join is not rejected. It must not be modified, but
   * it can be used to make the decision whether to reject the user join or
   * not dependent on the parameters, such as allowing the user join only if
   * the user has a predefined name. The function
   * inf_session_lookup_user_property() can be used to look up a named
   * parameter in the array.
   */
  session_proxy_signals[REJECT_USER_JOIN] = g_signal_new(
    "reject-user-join",
    G_OBJECT_CLASS_TYPE(object_class),
    G_SIGNAL_RUN_LAST,
    G_STRUCT_OFFSET(InfdSessionProxyClass, reject_user_join),
    g_signal_accumulator_true_handled, NULL,
    NULL,
    G_TYPE_BOOLEAN,
    3,
    INF_TYPE_XML_CONNECTION,
    G_TYPE_ARRAY,
    INF_TYPE_USER
  );
}

static void
infd_session_proxy_communication_object_iface_init(
  InfCommunicationObjectInterface* iface)
{
  iface->sent = infd_session_proxy_communication_object_sent;
  iface->enqueued = infd_session_proxy_communication_object_enqueued;
  iface->received = infd_session_proxy_communication_object_received;
}

static void
infd_session_proxy_session_proxy_iface_init(InfSessionProxyInterface* iface)
{
  iface->join_user = infd_session_proxy_session_proxy_join_user;
}

/*
 * Public API.
 */

/**
 * infd_session_proxy_subscribe_to:
 * @proxy: A #InfdSessionProxy.
 * @connection: A #InfXmlConnection that is not yet subscribed.
 * @seq_id: The sequence identifier for @connection.
 * @synchronize: If %TRUE, then synchronize the session to @connection first.
 *
 * Subscribes @connection to @proxy's session. The first thing that will be
 * done is a synchronization (see inf_session_synchronize_to()). Then, all
 * changes to the session are propagated to @connection.
 *
 * @seq_id should be a unique number for @connection, and the same number must
 * be passed on the client side to the #InfcSessionProxy object. Normally
 * #InfdDirectory and #InfcBrowser take care of choosing an appropriate
 * sequence identifier.
 *
 * Normally, you want to set @synchronize to %TRUE in which case the whole
 * session state will be synchronized to @connection (within the subscription
 * group). However, if for whatever reason the remote site already has a
 * copy of the session, then you may set @synchronize to %FALSE to skip
 * synchronization. This happens for example for newly created documents, or
 * when the remote site synchronized the local site and wants to be
 * initially subscribed.
 *
 * If @proxy's session is not in %INF_SESSION_RUNNING status, but in
 * %INF_SESSION_SYNCHRONIZING, then @connection must be the connection that
 * synchronizes the session and @synchronize needs to be set to %FALSE. This
 * causes the synchronizing connection to initially be subscribed. This
 * needs to be called directly after having created the session proxy (i.e.
 * without returning to the main loop before) so that the synchronization
 * connection is added to the subscription group for synchronization.
 *
 * Otherwise a subscription can only be initiated if @proxy's session is in
 * state %INF_SESSION_RUNNING.
 **/
void
infd_session_proxy_subscribe_to(InfdSessionProxy* proxy,
                                InfXmlConnection* connection,
                                guint seq_id,
                                gboolean synchronize)
{
  InfdSessionProxyPrivate* priv;

  g_return_if_fail(INFD_IS_SESSION_PROXY(proxy));
  g_return_if_fail(INF_IS_XML_CONNECTION(connection));

  g_return_if_fail(
    infd_session_proxy_find_subscription(proxy, connection) == NULL
  );

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);
  g_return_if_fail(priv->session != NULL);

  /* TODO: Also check connection against sync-conn in synchronizing case: */
  g_return_if_fail(
    inf_session_get_status(priv->session) == INF_SESSION_RUNNING ||
    (synchronize == FALSE)
  );

  /* Note we can't do this in the default signal handler since it doesn't
   * know the parent group. TODO: We can, meanwhile. */
  inf_communication_hosted_group_add_member(
    priv->subscription_group,
    connection
  );

  g_signal_emit(
    G_OBJECT(proxy),
    session_proxy_signals[ADD_SUBSCRIPTION],
    0,
    connection,
    seq_id
  );

  /* Make sure the default handler ran. Stopping the signal emission before
   * would leave us in an inconsistent state. */
  g_assert(infd_session_proxy_find_subscription(proxy, connection) != NULL);

  if(synchronize)
  {
    /* Directly synchronize within the subscription group so that we do not
     * need a group change after synchronization, and the connection already
     * receives requests from other group members to process after
     * synchronization. */
    inf_session_synchronize_to(
      priv->session,
      INF_COMMUNICATION_GROUP(priv->subscription_group),
      connection
    );
  }
}

/**
 * infd_session_proxy_unsubscribe:
 * @proxy: A #InfdSessionProxy.
 * @connection: The #InfXmlConnection to unsubscribe.
 *
 * Unsubscribes a subscribed connection from @proxy's session. This will
 * prevent all users joined via @connection to continue modifying the
 * session's buffer, and it will cancel ongoing synchronization to
 * @connection, if not yet finished.
 */
void
infd_session_proxy_unsubscribe(InfdSessionProxy* proxy,
                               InfXmlConnection* connection)
{
  InfdSessionProxyPrivate* priv;
  InfdSessionProxySubscription* subscription;
  InfSessionSyncStatus status;
  xmlNodePtr xml;

  g_return_if_fail(INFD_IS_SESSION_PROXY(proxy));
  g_return_if_fail(INF_IS_XML_CONNECTION(connection));

  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  /* TODO: Can we support the SYNCHRONIZING case? In that case the session
   * will probably end up closed... */
  g_assert(inf_session_get_status(priv->session) == INF_SESSION_RUNNING);

  subscription = infd_session_proxy_find_subscription(proxy, connection);
  g_return_if_fail(subscription != NULL);

  status = inf_session_get_synchronization_status(
    priv->session,
    subscription->connection
  );

  /* If synchronization is still in progress, the default handler of
   * InfSession will cancel the synchronization in which case we do
   * not need to send an extra session-close message. */

  /* We send session_close when we are in AWAITING_ACK status. In
   * AWAITING_ACK status we cannot cancel the synchronization anymore
   * because everything has already been sent out. Therefore the client
   * will eventuelly get in RUNNING state when it receives this message,
   * and process it correctly. */
  if(status != INF_SESSION_SYNC_IN_PROGRESS)
  {
    xml = xmlNewNode(NULL, (const xmlChar*)"session-close");

    inf_communication_group_send_message(
      INF_COMMUNICATION_GROUP(priv->subscription_group),
      subscription->connection,
      xml
    );
  }
  else
  {
    /* In case we are synchronizing the client */
    inf_session_cancel_synchronization(
      priv->session,
      subscription->connection
    );
  }

  inf_communication_hosted_group_remove_member(
    priv->subscription_group,
    subscription->connection
  );
}

/**
 * infd_session_proxy_has_subscriptions:
 * @proxy: A #InfdSessionProxy.
 *
 * Returns whether there are subscribed connections to the session.
 *
 * Returns: Whether there are subscribed connections.
 **/
gboolean
infd_session_proxy_has_subscriptions(InfdSessionProxy* proxy)
{
  InfdSessionProxyPrivate* priv;

  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), FALSE);
  priv = INFD_SESSION_PROXY_PRIVATE(proxy);

  if(priv->subscriptions == NULL)
    return FALSE;

  return TRUE;
}

/**
 * infd_session_proxy_is_subscribed:
 * @proxy: A #InfdSessionProxy.
 * @connection: The connection to check for being subscribed.
 *
 * Returns %TRUE when @connection is subscribed to the session and %FALSE
 * otherwise.
 *
 * Returns: Whether @connection is subscribed.
 **/
gboolean
infd_session_proxy_is_subscribed(InfdSessionProxy* proxy,
                                 InfXmlConnection* connection)
{
  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), FALSE);
  g_return_val_if_fail(INF_IS_XML_CONNECTION(connection), FALSE);

  if(infd_session_proxy_find_subscription(proxy, connection) == NULL)
    return FALSE;

  return TRUE;
}

/**
 * infd_session_proxy_is_idle:
 * @proxy: A #InfdSessionProxy.
 *
 * Returns whether the session is idle. The session is considered idle when
 * there are no subscriptions and no synchronizations (in either direction).
 *
 * Returns: Whether the session is idle.
 **/
gboolean
infd_session_proxy_is_idle(InfdSessionProxy* proxy)
{
  g_return_val_if_fail(INFD_IS_SESSION_PROXY(proxy), FALSE);
  return INFD_SESSION_PROXY_PRIVATE(proxy)->idle;
}

/* vim:set et sw=2 ts=2: */
