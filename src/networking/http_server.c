/*
 *  Copyright (C) 2007-2015 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */
#include <assert.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>

#include <libavutil/base64.h>

#define hsprintf(fmt...)  // TRACE(TRACE_DEBUG, "HTTP", fmt)

#include "http.h"
#include "http_server.h"
#include "misc/sha.h"
#include "prop/prop.h"
#include "arch/arch.h"
#include "asyncio.h"

#include "upnp/upnp.h"

static LIST_HEAD(, http_path) http_paths;
LIST_HEAD(http_connection_list, http_connection); 
int http_server_port;

/**
 *
 */
typedef struct http_path {
  LIST_ENTRY(http_path) hp_link;
  const char *hp_path;
  void *hp_opaque;
  http_callback_t *hp_callback;
  int hp_len;
  int hp_leaf;
  websocket_callback_init_t *hp_ws_init;
  websocket_callback_data_t *hp_ws_data;
  websocket_callback_fini_t *hp_ws_fini;
} http_path_t;


/**
 *
 */
struct http_connection {

  asyncio_fd_t *hc_afd;

  int hc_state;
#define HCS_COMMAND   0
#define HCS_HEADERS   1
#define HCS_POSTDATA  2
#define HCS_WEBSOCKET 3

  struct http_header_list hc_request_headers;
  struct http_header_list hc_req_args;
  struct http_header_list hc_response_headers;

  htsbuf_queue_t hc_output;

  http_cmd_t hc_cmd;

  enum {
    HTTP_VERSION_1_0,
    HTTP_VERSION_1_1,
    HTTP_VERSION_unknown = -1,
  } hc_version;

  char *hc_url;
  char *hc_url_orig;
  
  char hc_keep_alive;
  char hc_no_output;


  char *hc_post_data;
  size_t hc_post_len;
  size_t hc_post_offset;


  net_addr_t hc_local_addr;

  const http_path_t *hc_path;
  void *hc_opaque;

  char hc_my_addr[128]; // hc_local_addr as text
};


/**
 *
 */
static struct strtab HTTP_cmdtab[] = {
  { "GET",         HTTP_CMD_GET },
  { "HEAD",        HTTP_CMD_HEAD },
  { "POST",        HTTP_CMD_POST },
  { "SUBSCRIBE",   HTTP_CMD_SUBSCRIBE },
  { "UNSUBSCRIBE", HTTP_CMD_UNSUBSCRIBE },
};


/**
 *
 */
static struct strtab HTTP_versiontab[] = {
  { "HTTP/1.0",        HTTP_VERSION_1_0 },
  { "HTTP/1.1",        HTTP_VERSION_1_1 },
};


static asyncio_fd_t *http_server_fd;

static int http_write(http_connection_t *hc);

/**
 *
 */
void *
http_get_post_data(http_connection_t *hc, size_t *sizep, int steal)
{
  void *r = hc->hc_post_data;
  if(sizep != NULL)
    *sizep = hc->hc_post_len;
  if(steal)
    hc->hc_post_data = NULL;
  return r;
}


/**
 *
 */
static int
hp_cmp(const http_path_t *a, const http_path_t *b)
{
  return b->hp_len - a->hp_len;
}

/**
 * Add a callback for a given "virtual path" on our HTTP server
 */
void *
http_path_add(const char *path, void *opaque, http_callback_t *callback,
	      int leaf)
{
  http_path_t *hp = malloc(sizeof(http_path_t));

  hp->hp_len = strlen(path);
  hp->hp_path = strdup(path);
  hp->hp_opaque = opaque;
  hp->hp_callback = callback;
  hp->hp_leaf = leaf;
  LIST_INSERT_SORTED(&http_paths, hp, hp_link, hp_cmp, http_path_t);
  return hp;
}


/**
 *
 */
void *
http_add_websocket(const char *path,
		   websocket_callback_init_t *init,
		   websocket_callback_data_t *data,
		   websocket_callback_fini_t *fini)
{
  http_path_t *hp = malloc(sizeof(http_path_t));

  hp->hp_len = strlen(path);
  hp->hp_path = strdup(path);
  hp->hp_ws_init = init;
  hp->hp_ws_data = data;
  hp->hp_ws_fini = fini;
  hp->hp_leaf = 2;
  LIST_INSERT_HEAD(&http_paths, hp, hp_link);
  return hp;  
}

/**
 *
 */
static const http_path_t *
http_resolve(http_connection_t *hc, char **remainp, char **argsp)
{
  http_path_t *hp;
  char *v;

  LIST_FOREACH(hp, &http_paths, hp_link) {
    if(!strncmp(hc->hc_url, hp->hp_path, hp->hp_len)) {
      if(hc->hc_url[hp->hp_len] == 0 || hc->hc_url[hp->hp_len] == '/' ||
	 hc->hc_url[hp->hp_len] == '?')
	break;
    }
  }

  if(hp == NULL)
    return NULL;

  v = hc->hc_url + hp->hp_len;

  *remainp = NULL;
  *argsp = NULL;

  switch(*v) {
  case 0:
    break;

  case '/':
    if(v[1] == '?') {
      *argsp = v + 1;
      break;
    }

    *remainp = v + 1;
    v = strchr(v + 1, '?');
    if(v != NULL) {
      *v = 0;  /* terminate remaining url */
      *argsp = v + 1;
    }
    break;

  case '?':
    *argsp = v + 1;
    break;

  default:
    return NULL;
  }

  return hp;
}


/**
 * HTTP status code to string
 */
static const char *
http_rc2str(int code)
{
  switch(code) {
  case HTTP_STATUS_OK:              return "Ok";
  case HTTP_STATUS_NOT_FOUND:       return "Not found";
  case HTTP_STATUS_UNAUTHORIZED:    return "Unauthorized";
  case HTTP_STATUS_BAD_REQUEST:     return "Bad request";
  case HTTP_STATUS_FOUND:           return "Found";
  case HTTP_STATUS_METHOD_NOT_ALLOWED: return "Method not allowed";
  case HTTP_STATUS_PRECONDITION_FAILED: return "Precondition failed";
  case HTTP_STATUS_UNSUPPORTED_MEDIA_TYPE: return "Unsupported media type";
  case HTTP_NOT_IMPLEMENTED: return "Not implemented";
  case 500: return "Internal Server Error";
  default:
    return "Unknown returncode";
    break;
  }
}

/**
 * Transmit a HTTP reply
 */
static void
http_send_header(http_connection_t *hc, int rc, const char *content, 
		 int contentlen, const char *encoding, const char *location, 
		 int maxage, const char *range)
{
  htsbuf_queue_t hdrs;
  time_t t;
  http_header_t *hh;
  char date[64];

  time(&t);

  htsbuf_queue_init(&hdrs, 0);

  htsbuf_qprintf(&hdrs, "%s %d %s\r\n", 
		 val2str(hc->hc_version, HTTP_versiontab),
		 rc, http_rc2str(rc));

  htsbuf_qprintf(&hdrs, "Server: "APPNAMEUSER" %s\r\n",
		 htsversion_full);

  htsbuf_qprintf(&hdrs, "Date: %s\r\n", http_asctime(t, date, sizeof(date)));

  if(maxage == 0) {
    htsbuf_qprintf(&hdrs, "Cache-Control: no-cache\r\n");
  } else {

    htsbuf_qprintf(&hdrs,  "Last-Modified: %s\r\n",
		   http_asctime(t, date, sizeof(date)));

    t += maxage;

    htsbuf_qprintf(&hdrs, "Expires: %s\r\n",
		   http_asctime(t, date, sizeof(date)));
      
    htsbuf_qprintf(&hdrs, "Cache-Control: max-age=%d\r\n", maxage);
  }

  htsbuf_qprintf(&hdrs, "Connection: %s\r\n", 
		 hc->hc_keep_alive ? "Keep-Alive" : "Close");

  if(encoding != NULL)
    htsbuf_qprintf(&hdrs, "Content-Encoding: %s\r\n", encoding);

  if(location != NULL)
    htsbuf_qprintf(&hdrs, "Location: %s\r\n", location);

  if(content != NULL)
    htsbuf_qprintf(&hdrs, "Content-Type: %s\r\n", content);

  htsbuf_qprintf(&hdrs, "Content-Length: %d\r\n", contentlen);

  LIST_FOREACH(hh, &hc->hc_response_headers, hh_link)
    htsbuf_qprintf(&hdrs, "%s: %s\r\n", hh->hh_key, hh->hh_value);
  
  htsbuf_qprintf(&hdrs, "\r\n");
  
  htsbuf_appendq(&hc->hc_output, &hdrs);
}


/**
 * Transmit a HTTP reply
 */
int
http_send_raw(http_connection_t *hc, int rc, const char *rctxt,
	      struct http_header_list *headers, htsbuf_queue_t *output)
{
  htsbuf_queue_t hdrs;
  http_header_t *hh;
#if 0
  struct tm tm0, *tm;
  time_t t;
#endif
  htsbuf_queue_init(&hdrs, 0);

  htsbuf_qprintf(&hdrs, "%s %d %s\r\n", 
		 val2str(hc->hc_version, HTTP_versiontab),
		 rc, rctxt);

  if(headers != NULL) {
    LIST_FOREACH(hh, headers, hh_link)
      htsbuf_qprintf(&hdrs, "%s: %s\r\n", hh->hh_key, hh->hh_value);
    http_headers_free(headers);
  }

  htsbuf_qprintf(&hdrs, "\r\n");
  
  htsbuf_appendq(&hc->hc_output, &hdrs);

  if(output != NULL) {
    if(hc->hc_no_output)
      htsbuf_queue_flush(output);
    else
      htsbuf_appendq(&hc->hc_output, output);
  }
  return 0;
}


/**
 * Transmit a HTTP reply
 */
int
http_send_reply(http_connection_t *hc, int rc, const char *content, 
		const char *encoding, const char *location, int maxage,
		htsbuf_queue_t *output)
{

  http_send_header(hc, rc ?: 200, content, output ? output->hq_size : 0,
		   encoding, location, maxage, 0);

  if(output != NULL) {
    if(hc->hc_no_output)
      htsbuf_queue_flush(output);
    else
      htsbuf_appendq(&hc->hc_output, output);
  }
  return 0;
}


/**
 * Send HTTP error back
 */
int
http_error(http_connection_t *hc, int error, const char *fmt, ...)
{
  const char *errtxt = http_rc2str(error);
  htsbuf_queue_t hq;
  va_list ap;
  char extra[200];

  htsbuf_queue_init(&hq, 0);

  if(fmt != NULL) {
    va_start(ap, fmt);
    vsnprintf(extra, sizeof(extra), fmt, ap);
    va_end(ap);
  } else {
    extra[0] = 0;
  }


  TRACE(TRACE_ERROR, "HTTPSRV", "%d %s%s%s", error, hc->hc_url_orig,
	*extra ? " -- " : "", extra),


    htsbuf_qprintf(&hq, 
		   "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		   "<HTML><HEAD>\r\n"
		   "<TITLE>%d %s</TITLE>\r\n"
		   "</HEAD><BODY>\r\n"
		   "<H1>%d %s</H1>\r\n"
		   "<p>%s</p>\r\n"
		   "</BODY></HTML>\r\n",
		   error, errtxt, error, errtxt, extra);

  return http_send_reply(hc, error, "text/html", NULL, NULL, 0, &hq);
}


/**
 * Send an HTTP REDIRECT
 */
int
http_redirect(http_connection_t *hc, const char *location)
{
  htsbuf_queue_t hq;

  htsbuf_queue_init(&hq, 0);

  htsbuf_qprintf(&hq,
		 "<!DOCTYPE HTML PUBLIC \"-//IETF//DTD HTML 2.0//EN\">\r\n"
		 "<HTML><HEAD>\r\n"
		 "<TITLE>Redirect</TITLE>\r\n"
		 "</HEAD><BODY>\r\n"
		 "Please follow <a href=\"%s\">%s</a>\r\n"
		 "</BODY></HTML>\r\n",
		 location, location);

  return http_send_reply(hc, HTTP_STATUS_FOUND,
			 "text/html", NULL, location, 0, &hq);
}


/**
 *
 */
static void
http_exec(http_connection_t *hc, const http_path_t *hp, char *remain,
	  http_cmd_t method)
{
  hsprintf("%p: Dispatching [%s] on thread 0x%lx\n",
           hc, hp->hp_path, (unsigned long)pthread_self());
  int err = hp->hp_callback(hc, remain, hp->hp_opaque, method);
  hsprintf("%p: Returned from fn, err = %d\n", hc, err);

  if(err == HTTP_STATUS_OK) {
    htsbuf_queue_t out;
    htsbuf_queue_init(&out, 0);
    htsbuf_append(&out, "OK\n", 3);
    http_send_reply(hc, 0, "text/plain", NULL, NULL, 0, &out);
    return;
  } else if(err > 0)
    http_error(hc, err, NULL);
  else if(err == 0)
    return;
  else
    abort();
}


/**
 *
 */
const char *
http_arg_get_req(http_connection_t *hc, const char *name)
{
  return http_header_get(&hc->hc_req_args, name);
}


/**
 *
 */
const char *
http_arg_get_hdr(http_connection_t *hc, const char *name)
{
  return http_header_get(&hc->hc_request_headers, name);
}


/**
 *
 */
void
http_set_response_hdr(http_connection_t *hc, const char *name,
		      const char *value)
{
  http_header_add(&hc->hc_response_headers, name, value, 0);
}


/**
 * Split a string in components delimited by 'delimiter'
 */
static int
http_tokenize(char *buf, char **vec, int vecsize, int delimiter)
{
  int n = 0;

  while(1) {
    while((*buf > 0 && *buf < 33) || *buf == delimiter)
      buf++;
    if(*buf == 0)
      break;
    vec[n++] = buf;
    if(n == vecsize)
      break;
    while(*buf > 32 && *buf != delimiter)
      buf++;
    if(*buf == 0)
      break;
    *buf = 0;
    buf++;
  }
  return n;
}



#define WSGUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/**
 *
 */
static int
http_cmd_start_websocket(http_connection_t *hc, const http_path_t *hp)
{
  sha1_decl(shactx);
  char sig[64];
  uint8_t d[20];
  struct http_header_list headers;
  int err;
  const char *k = http_header_get(&hc->hc_request_headers, "Sec-WebSocket-Key");

  if(k == NULL) {
    http_error(hc, HTTP_STATUS_BAD_REQUEST, NULL);
    return 0;
  }

  hc->hc_opaque = NULL;
  if((err = hp->hp_ws_init(hc)) != 0)
    return http_error(hc, err, NULL);

  sha1_init(shactx);
  sha1_update(shactx, (const uint8_t *)k, strlen(k));
  sha1_update(shactx, (const uint8_t *)WSGUID, strlen(WSGUID));
  sha1_final(shactx, d);

  av_base64_encode(sig, sizeof(sig), d, 20);

  LIST_INIT(&headers);

  http_header_add(&headers, "Connection", "Upgrade", 0);
  http_header_add(&headers, "Upgrade", "websocket", 0);
  http_header_add(&headers, "Sec-WebSocket-Accept", sig, 0);

  http_send_raw(hc, 101, "Switching Protocols", &headers, NULL);
  hc->hc_state = HCS_WEBSOCKET;
 
  hc->hc_path = hp;
  return 0;
}


/**
 *
 */
static int
http_cmd_get(http_connection_t *hc, http_cmd_t method)
{
  const http_path_t *hp;
  char *remain;
  char *args;

  hp = http_resolve(hc, &remain, &args);
  if(hp == NULL || (hp->hp_leaf && remain != NULL)) {
    http_error(hc, HTTP_STATUS_NOT_FOUND, NULL);
    return 0;
  }

  if(args != NULL)
    http_parse_uri_args(&hc->hc_req_args, args, 0);

  const char *c = http_header_get(&hc->hc_request_headers, "Connection");
  const char *u = http_header_get(&hc->hc_request_headers, "Upgrade");

  if(c && u && !strcasecmp(c, "Upgrade") && !strcasecmp(u, "websocket")) {
    if(hp->hp_leaf != 2) {
      http_error(hc, HTTP_STATUS_METHOD_NOT_ALLOWED, NULL);
      return 0;
    }
    return http_cmd_start_websocket(hc, hp);
  }

  if(hp->hp_leaf == 2) {
    // Websocket endpoint don't wanna deal with normal HTTP requests
    http_error(hc, HTTP_STATUS_METHOD_NOT_ALLOWED, NULL);
    return 0;
  }

  http_exec(hc, hp, remain, method);
  return 0;
}


/**
 *
 */
static int
http_read_post(http_connection_t *hc, htsbuf_queue_t *q)
{
  const http_path_t *hp;
  const char *content_type;
  char *v, *argv[2], *args, *remain;
  int n;
  size_t size = q->hq_size;
  size_t rsize = hc->hc_post_len - hc->hc_post_offset;

  if(size > rsize)
    size = rsize;

  n = htsbuf_read(q, hc->hc_post_data + hc->hc_post_offset, size);
  assert(n == size);

  hc->hc_post_offset += size;
  assert(hc->hc_post_offset <= hc->hc_post_len);

  if(hc->hc_post_offset < hc->hc_post_len)
    return 0;

  hc->hc_state = HCS_COMMAND;

  /* Parse content-type */
  content_type = http_header_get(&hc->hc_request_headers, "Content-Type");

  if(content_type != NULL) {

    v = mystrdupa(content_type);

    n = http_tokenize(v, argv, 2, ';');
    if(n == 0) {
      http_error(hc, HTTP_STATUS_BAD_REQUEST, "Content-Type malformed");
      return 0;
    }

    if(!strcmp(argv[0], "application/x-www-form-urlencoded"))
      http_parse_uri_args(&hc->hc_req_args, hc->hc_post_data, 0);
  }

  hp = http_resolve(hc, &remain, &args);
  if(hp == NULL) {
    http_error(hc, HTTP_STATUS_NOT_FOUND, NULL);
    return 0;
  }
  http_exec(hc, hp, remain, HTTP_CMD_POST);
  return 0;
}


/**
 *
 */
static int
http_cmd_post(http_connection_t *hc, htsbuf_queue_t *q)
{
  const char *v;

  v = http_header_get(&hc->hc_request_headers, "Content-Length");
  if(v == NULL) {
    /* No content length in POST, make us disconnect */
    return 1;
  }

  hc->hc_post_len = atoi(v);
  if(hc->hc_post_len > 16 * 1024 * 1024) {
    /* Bail out if POST data > 16 Mb */
    hc->hc_keep_alive = 0;
    return 1;
  }

  /* Allocate space for data, we add a terminating null char to ease
     string processing on the content */

  hc->hc_post_data = malloc(hc->hc_post_len + 1);
  if(hc->hc_post_data == NULL) {
    hc->hc_keep_alive = 0;
    return 1;
  }

  hc->hc_post_data[hc->hc_post_len] = 0;
  hc->hc_post_offset = 0;

  v = http_header_get(&hc->hc_request_headers, "Expect");
  if(v != NULL) {
    if(!strcasecmp(v, "100-continue")) {
      htsbuf_qprintf(&hc->hc_output, "HTTP/1.1 100 Continue\r\n\r\n");
    }
  }


  hc->hc_state = HCS_POSTDATA;
  return http_read_post(hc, q);
}

/**
 *
 */
static int
http_handle_request(http_connection_t *hc, htsbuf_queue_t *q)
{
  hc->hc_state = HCS_COMMAND;
  /* Set keep-alive status */
  const char *v = http_header_get(&hc->hc_request_headers, "connection");

  switch(hc->hc_version) {
  case HTTP_VERSION_1_0:
    /* Keep-alive is default off, but can be enabled */
    hc->hc_keep_alive = v != NULL && !strcasecmp(v, "keep-alive");
    break;

  case HTTP_VERSION_1_1:
    /* Keep-alive is default on, but can be disabled */
    hc->hc_keep_alive = !(v != NULL && !strcasecmp(v, "close"));
    break;

  default:
    http_error(hc, HTTP_NOT_IMPLEMENTED, NULL);
    return 0;
  }

  hc->hc_no_output = hc->hc_cmd == HTTP_CMD_HEAD;

  switch(hc->hc_cmd) {
  default:
    http_error(hc, HTTP_NOT_IMPLEMENTED, NULL);
    return 0;
    
  case HTTP_CMD_POST:
    return http_cmd_post(hc, q);

  case HTTP_CMD_HEAD:
    hc->hc_no_output = 1;
    // FALLTHRU
  case HTTP_CMD_GET:
  case HTTP_CMD_SUBSCRIBE:
  case HTTP_CMD_UNSUBSCRIBE:
    return http_cmd_get(hc, hc->hc_cmd);
  }
  return 1;
}


/**
 *
 */
static int
http_read_line(htsbuf_queue_t *q, char *buf, size_t bufsize)
{
  int len;

  len = htsbuf_find(q, 0xa);
  if(len == -1)
    return 0;

  if(len >= bufsize - 1)
    return -1;

  htsbuf_read(q, buf, len);
  buf[len] = 0;
  while(len > 0 && buf[len - 1] < 32)
    buf[--len] = 0;
  htsbuf_drop(q, 1); /* Drop the \n */
  return 1;
}


/**
 *
 */
void
http_set_opaque(http_connection_t *hc, void *opaque)
{
  hc->hc_opaque = opaque;
}


/**
 *
 */
static void
websocket_send_hdr(http_connection_t *hc, int opcode, size_t len)
{
  uint8_t hdr[14]; // max header length
  int hlen;
  hdr[0] = 0x80 | (opcode & 0xf);
  if(len <= 125) {
    hdr[1] = len;
    hlen = 2;
  } else if(len < 65536) {
    hdr[1] = 126;
    hdr[2] = len >> 8;
    hdr[3] = len;
    hlen = 4;
  } else {
    hdr[1] = 127;
    uint64_t u64 = len;
#if defined(__LITTLE_ENDIAN__)
    u64 = __builtin_bswap64(u64);
#endif
    memcpy(hdr + 2, &u64, sizeof(uint64_t));
    hlen = 10;
  }

  htsbuf_append(&hc->hc_output, hdr, hlen);
}


/**
 *
 */
void
websocket_send(http_connection_t *hc, int opcode, const void *data,
	       size_t len)
{
  websocket_send_hdr(hc, opcode, len);
  htsbuf_append(&hc->hc_output, data, len);
  http_write(hc);
}


/**
 *
 */
void
websocket_sendq(http_connection_t *hc, int opcode, htsbuf_queue_t *hq)
{
  websocket_send_hdr(hc, opcode, hq->hq_size);
  htsbuf_appendq(&hc->hc_output, hq);
  http_write(hc);
}



/**
 *
 */
static int
websocket_input(http_connection_t *hc, htsbuf_queue_t *q)
{
  uint8_t hdr[14]; // max header length
  int p = htsbuf_peek(q, &hdr, 14);
  const uint8_t *m;

  if(p < 2)
    return 0;

  int opcode  = hdr[0] & 0xf;
  int64_t len = hdr[1] & 0x7f;
  int hoff = 2;

  if(len == 126) {
    if(p < 4)
      return 0;
    len = hdr[2] << 8 | hdr[3];
    hoff = 4;
  } else if(len == 127) {
    if(p < 10)
      return 0;
    memcpy(&len, hdr + 2, sizeof(uint64_t));
#if defined(__LITTLE_ENDIAN__)
    len = __builtin_bswap64(len);
#endif
    hoff = 10;
  }

  if(hdr[1] & 0x80) {
    if(p < hoff + 4)
      return 0;
    m = hdr + hoff;

    hoff += 4;
  } else {
    m = NULL;
  }

  if(q->hq_size < hoff + len)
    return 0;

  uint8_t *d = mymalloc(len+1);
  if(d == NULL)
    return 1;

  htsbuf_drop(q, hoff);
  htsbuf_read(q, d, len);
  d[len] = 0;

  if(m != NULL) {
    int i;
    for(i = 0; i < len; i++)
      d[i] ^= m[i&3];
  }
  

  if(opcode == 9) {
    // PING
    websocket_send(hc, 10, d, len);
  } else {
    hc->hc_path->hp_ws_data(hc, opcode, d, len, hc->hc_opaque);
  }
  free(d);
  return -1;
}



/**
 *
 */
static int
http_handle_input(http_connection_t *hc, htsbuf_queue_t *q)
{
  char buf[1024];
  char *argv[3], *c;
  int r, n;

  while(1) {

    switch(hc->hc_state) {
    case HCS_COMMAND:
      free(hc->hc_post_data);
      hc->hc_post_data = NULL;

      r = http_read_line(q, buf, sizeof(buf));

      if(r == -1)
	return 1;

      if(r == 0)
	return 0;

      hsprintf("%p: %s\n", hc, buf);

      if((n = http_tokenize(buf, argv, 3, -1)) != 3)
	return 1;

      hc->hc_cmd = str2val(argv[0], HTTP_cmdtab);

      mystrset(&hc->hc_url, argv[1]);
      mystrset(&hc->hc_url_orig, argv[1]);

      if((hc->hc_version = str2val(argv[2], HTTP_versiontab)) == -1)
	return 1;

      hc->hc_state = HCS_HEADERS;
      /* FALLTHRU */

      http_headers_free(&hc->hc_req_args);
      http_headers_free(&hc->hc_request_headers);
      http_headers_free(&hc->hc_response_headers);

    case HCS_HEADERS:
      if((r = http_read_line(q, buf, sizeof(buf))) == -1)
	return 1;

      if(r == 0)
	return 0;

      hsprintf("%p: %s\n", hc, buf);

      if(buf[0] == 32 || buf[0] == '\t') {
	// LWS

	http_header_add_lws(&hc->hc_request_headers, buf+1);

      } else if(buf[0] == 0) {
	
	if(http_handle_request(hc, q))
	  return 1;

	if(TAILQ_FIRST(&hc->hc_output.hq_q) == NULL && !hc->hc_keep_alive)
	  return 1;

      } else {

	if((c = strchr(buf, ':')) == NULL)
	  return 1;
	*c++ = 0;
	while(*c == 32)
	  c++;
	http_header_add(&hc->hc_request_headers, buf, c, 0);
      }
      break;

    case HCS_POSTDATA:
      if(!http_read_post(hc, q))
	return 0;
      break;

    case HCS_WEBSOCKET:
      if((r = websocket_input(hc, q)) != -1)
	return r;
      break;
    }
  }
}


/**
 *
 */
static int
http_write(http_connection_t *hc)
{
  asyncio_sendq(hc->hc_afd, &hc->hc_output, 0);
  return 0;
}


/**
 *
 */
static void
http_close(http_connection_t *hc)
{
  hsprintf("%p: ----------------- CLOSED CONNECTION\n", hc);
  htsbuf_queue_flush(&hc->hc_output);
  http_headers_free(&hc->hc_req_args);
  http_headers_free(&hc->hc_request_headers);
  http_headers_free(&hc->hc_response_headers);
  asyncio_del_fd(hc->hc_afd);
  free(hc->hc_url);
  free(hc->hc_url_orig);
  free(hc->hc_post_data);

  if(hc->hc_path != NULL && hc->hc_path->hp_ws_fini != NULL)
    hc->hc_path->hp_ws_fini(hc, hc->hc_opaque);
  free(hc);
}


/**
 *
 */
static void
http_io_error(void *opaque, const char *error)
{
  http_close(opaque);
}


/**
 *
 */
static void
http_io_read(void *opaque, htsbuf_queue_t *q)
{
  http_connection_t *hc = opaque;
  http_handle_input(hc, q);
  http_write(hc);
}


/**
 *
 */
const char *
http_get_my_host(http_connection_t *hc)
{
  if(!hc->hc_my_addr)
    net_fmt_host(hc->hc_my_addr, sizeof(hc->hc_my_addr), &hc->hc_local_addr);
  return hc->hc_my_addr;
}


/**
 *
 */
int
http_get_my_port(http_connection_t *hc)
{
  return hc->hc_local_addr.na_port;
}


/**
 *
 */
static void
http_accept(void *opaque, int fd, const net_addr_t *local_addr,
            const net_addr_t *remote_addr)
{
  http_connection_t *hc = calloc(1, sizeof(http_connection_t));
  hc->hc_afd = asyncio_attach("HTTP connection", fd,
                              http_io_error, http_io_read, hc);
  htsbuf_queue_init(&hc->hc_output, 0);

  hc->hc_local_addr  = *local_addr;
}


/**
 *
 */
static void
http_server_init(void)
{
  http_server_fd = asyncio_listen("http-server",
                                  42000,
                                  http_accept,
                                  NULL, 1);

#if STOS
  asyncio_listen("http-server", 80, http_accept, NULL, 1);
#endif

  if(http_server_fd != NULL) {
    http_server_port = asyncio_get_port(http_server_fd);

#if ENABLE_UPNP
    if(!gconf.disable_upnp)
      upnp_init();
#endif

  }
}

/**
 *
 */
void
http_req_args_fill_htsmsg(http_connection_t *hc, htsmsg_t *msg)
{
  http_header_t *hh;

  LIST_FOREACH(hh, &hc->hc_req_args, hh_link)
    htsmsg_add_str(msg, hh->hh_key, hh->hh_value);
}

INITME(INIT_GROUP_ASYNCIO, http_server_init, NULL);
