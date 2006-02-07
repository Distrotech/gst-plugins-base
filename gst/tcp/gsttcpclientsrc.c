/* GStreamer
 * Copyright (C) <1999> Erik Walthinsen <omega@cse.ogi.edu>
 * Copyright (C) <2004> Thomas Vander Stichele <thomas at apestaart dot org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst-i18n-plugin.h>
#include "gsttcp.h"
#include "gsttcpclientsrc.h"
#include <string.h>             /* memset */
#include <unistd.h>
#include <arpa/inet.h>
#include <fcntl.h>


/* control stuff stolen from fdsrc */
#define CONTROL_STOP            'S'     /* stop the select call */
#define CONTROL_SOCKETS(o)      o->control_fds
#define WRITE_SOCKET(o)         o->control_fds[1]
#define READ_SOCKET(o)          o->control_fds[0]

#define SEND_COMMAND(o, command)          \
G_STMT_START {                              \
  unsigned char c; c = command;             \
  write (WRITE_SOCKET(o), &c, 1);         \
} G_STMT_END

#define READ_COMMAND(o, command, res)        \
G_STMT_START {                                 \
  res = read(READ_SOCKET(o), &command, 1);   \
} G_STMT_END


GST_DEBUG_CATEGORY (tcpclientsrc_debug);
#define GST_CAT_DEFAULT tcpclientsrc_debug

#define MAX_READ_SIZE                   4 * 1024


static GstElementDetails gst_tcp_client_src_details =
GST_ELEMENT_DETAILS ("TCP Client source",
    "Source/Network",
    "Receive data as a client over the network via TCP",
    "Thomas Vander Stichele <thomas at apestaart dot org>");

static GstStaticPadTemplate srctemplate = GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


enum
{
  PROP_0,
  PROP_HOST,
  PROP_PORT,
  PROP_PROTOCOL
};


GST_BOILERPLATE (GstTCPClientSrc, gst_tcp_client_src, GstPushSrc,
    GST_TYPE_PUSH_SRC);


static void gst_tcp_client_src_finalize (GObject * gobject);

static GstCaps *gst_tcp_client_src_getcaps (GstBaseSrc * psrc);

static GstFlowReturn gst_tcp_client_src_create (GstPushSrc * psrc,
    GstBuffer ** outbuf);
static gboolean gst_tcp_client_src_stop (GstBaseSrc * bsrc);
static gboolean gst_tcp_client_src_start (GstBaseSrc * bsrc);
static gboolean gst_tcp_client_src_unlock (GstBaseSrc * bsrc);

static void gst_tcp_client_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_tcp_client_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);


static void
gst_tcp_client_src_base_init (gpointer g_class)
{
  GstElementClass *element_class = GST_ELEMENT_CLASS (g_class);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&srctemplate));

  gst_element_class_set_details (element_class, &gst_tcp_client_src_details);
}

static void
gst_tcp_client_src_class_init (GstTCPClientSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpush_src_class;

  gobject_class = (GObjectClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpush_src_class = (GstPushSrcClass *) klass;

  gobject_class->set_property = gst_tcp_client_src_set_property;
  gobject_class->get_property = gst_tcp_client_src_get_property;
  gobject_class->finalize = gst_tcp_client_src_finalize;

  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_HOST,
      g_param_spec_string ("host", "Host",
          "The host IP address to receive packets from", TCP_DEFAULT_HOST,
          G_PARAM_READWRITE));
  g_object_class_install_property (G_OBJECT_CLASS (klass), PROP_PORT,
      g_param_spec_int ("port", "Port", "The port to receive packets from", 0,
          TCP_HIGHEST_PORT, TCP_DEFAULT_PORT, G_PARAM_READWRITE));
  g_object_class_install_property (gobject_class, PROP_PROTOCOL,
      g_param_spec_enum ("protocol", "Protocol", "The protocol to wrap data in",
          GST_TYPE_TCP_PROTOCOL, GST_TCP_PROTOCOL_NONE, G_PARAM_READWRITE));

  gstbasesrc_class->get_caps = gst_tcp_client_src_getcaps;
  gstbasesrc_class->start = gst_tcp_client_src_start;
  gstbasesrc_class->stop = gst_tcp_client_src_stop;
  gstbasesrc_class->unlock = gst_tcp_client_src_unlock;

  gstpush_src_class->create = gst_tcp_client_src_create;

  GST_DEBUG_CATEGORY_INIT (tcpclientsrc_debug, "tcpclientsrc", 0,
      "TCP Client Source");
}

static void
gst_tcp_client_src_init (GstTCPClientSrc * this, GstTCPClientSrcClass * g_class)
{
  this->port = TCP_DEFAULT_PORT;
  this->host = g_strdup (TCP_DEFAULT_HOST);
  this->sock_fd = -1;
  this->protocol = GST_TCP_PROTOCOL_NONE;
  this->caps = NULL;

  READ_SOCKET (this) = -1;
  WRITE_SOCKET (this) = -1;

  gst_base_src_set_live (GST_BASE_SRC (this), TRUE);

  GST_OBJECT_FLAG_UNSET (this, GST_TCP_CLIENT_SRC_OPEN);
}

static void
gst_tcp_client_src_finalize (GObject * gobject)
{
  GstTCPClientSrc *this = GST_TCP_CLIENT_SRC (gobject);

  g_free (this->host);
}

static GstCaps *
gst_tcp_client_src_getcaps (GstBaseSrc * bsrc)
{
  GstTCPClientSrc *src;
  GstCaps *caps = NULL;

  src = GST_TCP_CLIENT_SRC (bsrc);

  if (!GST_OBJECT_FLAG_IS_SET (src, GST_TCP_CLIENT_SRC_OPEN))
    caps = gst_caps_new_any ();
  else if (src->caps)
    caps = gst_caps_copy (src->caps);
  else
    caps = gst_caps_new_any ();
  GST_DEBUG_OBJECT (src, "returning caps %" GST_PTR_FORMAT, caps);
  g_assert (GST_IS_CAPS (caps));
  return caps;
}

static GstFlowReturn
gst_tcp_client_src_create (GstPushSrc * psrc, GstBuffer ** outbuf)
{
  GstTCPClientSrc *src;
  GstFlowReturn ret = GST_FLOW_OK;
  GstBaseSrc *bsrc = GST_BASE_SRC (psrc);

  src = GST_TCP_CLIENT_SRC (psrc);

  if (!GST_OBJECT_FLAG_IS_SET (src, GST_TCP_CLIENT_SRC_OPEN))
    goto wrong_state;

  GST_LOG_OBJECT (src, "asked for a buffer");

  /* read the buffer header if we're using a protocol */
  switch (src->protocol) {
    case GST_TCP_PROTOCOL_NONE:
      ret = gst_tcp_read_buffer (GST_ELEMENT (src), src->sock_fd,
          READ_SOCKET (src), outbuf);
      break;

    case GST_TCP_PROTOCOL_GDP:
      /* get the caps if we're using GDP */
      if (!src->caps_received) {
        GstCaps *caps;

        GST_DEBUG_OBJECT (src, "getting caps through GDP");
        ret = gst_tcp_gdp_read_caps (GST_ELEMENT (src), src->sock_fd,
            READ_SOCKET (src), &caps);

        if (ret != GST_FLOW_OK)
          goto no_caps;

        src->caps_received = TRUE;
        src->caps = caps;
      }

      ret = gst_tcp_gdp_read_buffer (GST_ELEMENT (src), src->sock_fd,
          READ_SOCKET (src), outbuf);

      if (G_UNLIKELY (bsrc->segment.format != GST_FORMAT_TIME)
          && G_LIKELY (ret == GST_FLOW_OK)
          && G_LIKELY (GST_BUFFER_TIMESTAMP_IS_VALID (*outbuf))) {
        /* switch our format to time */
        gst_base_src_set_format (bsrc, GST_FORMAT_TIME);
        gst_segment_set_newsegment (&bsrc->segment, FALSE, 1.0,
            GST_FORMAT_TIME, GST_BUFFER_TIMESTAMP (*outbuf), -1, 0);

        gst_pad_push_event (bsrc->srcpad,
            gst_event_new_new_segment (FALSE,
                bsrc->segment.rate, bsrc->segment.format,
                bsrc->segment.start, bsrc->segment.stop, bsrc->segment.time));
      }

      break;
    default:
      /* need to assert as buf == NULL */
      g_assert ("Unhandled protocol type");
      break;
  }

  if (ret == GST_FLOW_OK) {
    GST_LOG_OBJECT (src,
        "Returning buffer from _get of size %d, ts %"
        GST_TIME_FORMAT ", dur %" GST_TIME_FORMAT
        ", offset %" G_GINT64_FORMAT ", offset_end %" G_GINT64_FORMAT,
        GST_BUFFER_SIZE (*outbuf),
        GST_TIME_ARGS (GST_BUFFER_TIMESTAMP (*outbuf)),
        GST_TIME_ARGS (GST_BUFFER_DURATION (*outbuf)),
        GST_BUFFER_OFFSET (*outbuf), GST_BUFFER_OFFSET_END (*outbuf));

    gst_buffer_set_caps (*outbuf, src->caps);
  }

  return ret;

wrong_state:
  {
    GST_DEBUG_OBJECT (src, "connection to closed, cannot read data");
    return GST_FLOW_WRONG_STATE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, READ, (NULL),
        ("Could not read caps through GDP"));
    return ret;
  }
}

static void
gst_tcp_client_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstTCPClientSrc *tcpclientsrc = GST_TCP_CLIENT_SRC (object);

  switch (prop_id) {
    case PROP_HOST:
      if (!g_value_get_string (value)) {
        g_warning ("host property cannot be NULL");
        break;
      }
      g_free (tcpclientsrc->host);
      tcpclientsrc->host = g_strdup (g_value_get_string (value));
      break;
    case PROP_PORT:
      tcpclientsrc->port = g_value_get_int (value);
      break;
    case PROP_PROTOCOL:
      tcpclientsrc->protocol = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_tcp_client_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstTCPClientSrc *tcpclientsrc = GST_TCP_CLIENT_SRC (object);

  switch (prop_id) {
    case PROP_HOST:
      g_value_set_string (value, tcpclientsrc->host);
      break;
    case PROP_PORT:
      g_value_set_int (value, tcpclientsrc->port);
      break;
    case PROP_PROTOCOL:
      g_value_set_enum (value, tcpclientsrc->protocol);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

/* create a socket for connecting to remote server */
static gboolean
gst_tcp_client_src_start (GstBaseSrc * bsrc)
{
  int ret;
  gchar *ip;
  GstTCPClientSrc *src = GST_TCP_CLIENT_SRC (bsrc);

  /* create the control sockets before anything */
  if (socketpair (PF_UNIX, SOCK_STREAM, 0, CONTROL_SOCKETS (src)) < 0)
    goto socket_pair;

  fcntl (READ_SOCKET (src), F_SETFL, O_NONBLOCK);
  fcntl (WRITE_SOCKET (src), F_SETFL, O_NONBLOCK);

  /* create receiving client socket */
  GST_DEBUG_OBJECT (src, "opening receiving client socket to %s:%d",
      src->host, src->port);

  if ((src->sock_fd = socket (AF_INET, SOCK_STREAM, 0)) == -1)
    goto no_socket;

  GST_DEBUG_OBJECT (src, "opened receiving client socket with fd %d",
      src->sock_fd);
  GST_OBJECT_FLAG_SET (src, GST_TCP_CLIENT_SRC_OPEN);

  /* look up name if we need to */
  if (!(ip = gst_tcp_host_to_ip (GST_ELEMENT (src), src->host)))
    goto name_resolv;

  GST_DEBUG_OBJECT (src, "IP address for host %s is %s", src->host, ip);

  /* connect to server */
  memset (&src->server_sin, 0, sizeof (src->server_sin));
  src->server_sin.sin_family = AF_INET; /* network socket */
  src->server_sin.sin_port = htons (src->port); /* on port */
  src->server_sin.sin_addr.s_addr = inet_addr (ip);     /* on host ip */
  g_free (ip);

  GST_DEBUG_OBJECT (src, "connecting to server");
  ret = connect (src->sock_fd, (struct sockaddr *) &src->server_sin,
      sizeof (src->server_sin));

  if (ret) {
    gst_tcp_client_src_stop (GST_BASE_SRC (src));
    switch (errno) {
      case ECONNREFUSED:
        GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ,
            (_("Connection to %s:%d refused."), src->host, src->port), (NULL));
        return FALSE;
        break;
      default:
        GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ, (NULL),
            ("connect to %s:%d failed: %s", src->host, src->port,
                g_strerror (errno)));
        return FALSE;
        break;
    }
  }

  return TRUE;

socket_pair:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ_WRITE, (NULL),
        GST_ERROR_SYSTEM);
    return FALSE;
  }
no_socket:
  {
    GST_ELEMENT_ERROR (src, RESOURCE, OPEN_READ, (NULL), GST_ERROR_SYSTEM);
    return FALSE;
  }
name_resolv:
  {
    gst_tcp_client_src_stop (GST_BASE_SRC (src));
    return FALSE;
  }
}

/* close the socket and associated resources
 * unset OPEN flag
 * used both to recover from errors and go to NULL state */
static gboolean
gst_tcp_client_src_stop (GstBaseSrc * bsrc)
{
  GstTCPClientSrc *src;

  src = GST_TCP_CLIENT_SRC (bsrc);

  GST_DEBUG_OBJECT (src, "closing socket");
  if (src->sock_fd != -1) {
    close (src->sock_fd);
    src->sock_fd = -1;
  }
  src->caps_received = FALSE;
  if (src->caps) {
    gst_caps_unref (src->caps);
    src->caps = NULL;
  }
  GST_OBJECT_FLAG_UNSET (src, GST_TCP_CLIENT_SRC_OPEN);

  close (READ_SOCKET (src));
  close (WRITE_SOCKET (src));
  READ_SOCKET (src) = -1;
  WRITE_SOCKET (src) = -1;

  return TRUE;
}

/* will be called only between calls to start() and stop() */
static gboolean
gst_tcp_client_src_unlock (GstBaseSrc * bsrc)
{
  GstTCPClientSrc *src = GST_TCP_CLIENT_SRC (bsrc);

  SEND_COMMAND (src, CONTROL_STOP);

  return TRUE;
}
