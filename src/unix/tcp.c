/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#include "uv.h"
#include "internal.h"

#include <stdlib.h>
#include <unistd.h>
#include <assert.h>
#include <errno.h>

#include <stdio.h>

//#include "ixev.h"

 #include <netinet/in.h>



int uv_tcp_init(uv_loop_t* loop, uv_tcp_t* tcp) {
  uv__stream_init(loop, (uv_stream_t*)tcp, UV_TCP);
  tcp->_ixev_ctx = NULL;
  return 0;
}

static uv_tcp_t* singleton_listening_socket = NULL;

void ixuv__put_binding(unsigned long addr, unsigned short port, uv_tcp_t* tcp){
  singleton_listening_socket = tcp;
}
uv_tcp_t* ixuv__get_binding(unsigned long addr, unsigned short port){
  return singleton_listening_socket;
}

void ixuv__enqueue_ctx(uv_tcp_t* tcp, ixev_ctx* ctx){
  ctx_post* post = (ctx_post*) malloc(sizeof(ctx_post));
  post->_ixev_ctx = ctx;
  post->next = NULL;
  if(tcp->pending_ctx_head == NULL){
    tcp->pending_ctx_head = post;
    tcp->pending_ctx_tail = post;
  }
  tcp->pending_ctx_tail->next = post;
  tcp->pending_ctx_tail = post;

}
ixev_ctx* ixuv__dequeue_ctx(uv_tcp_t* tcp){
  if(tcp->pending_ctx_head == NULL){
    return NULL;
  }
  ctx_post* post = tcp->pending_ctx_head;
  ixev_ctx* ixev_ctx_ = post -> _ixev_ctx; 
  if (  tcp->pending_ctx_head ==  tcp->pending_ctx_tail) { 
     tcp->pending_ctx_head = NULL;
     tcp->pending_ctx_tail = NULL;
  } else {
     tcp->pending_ctx_head = post->next; //Maybe this line suffices
  }
  free(post);
  return ixev_ctx_;
}

static void ixuv__multiplex_handler(struct ixev_ctx *ctx, unsigned int reason) {
    printf("ixuv__multiplex_handler, reason: %d (IXEVIN: %d, IXEVOUT: %d, IXEVHUP %d)\n", reason, IXEVIN, IXEVOUT,IXEVHUP);
    uv_tcp_t* stream = (uv_tcp_t*) ctx->user_data;
    if(stream==NULL){
      fprintf(stderr, "Null context from ctx handler - in need of a queue?\n");
      return;
    }
    if(reason == IXEVIN && stream->read_cb != NULL){
        uv_buf_t buf;
        stream->alloc_cb((uv_handle_t*)stream, 64 * 1024, &buf);
        int read = ixev_recv(ctx, buf.base, buf.len);
        // if(stream->read_cb != NULL){ // @TODO: Maybe this should enclose the read from ix as well.
        stream->read_cb((uv_stream_t*)stream, read, &buf);
        // }
    } 

}

struct ixev_ctx *ixuv__accept(struct ip_tuple *id)
{
  
  // struct in_addr src, dst;
  // src.s_addr = id->src_ip;
  // dst.s_addr = id->dst_ip;
  // printf("Got conn from: %s:%d to %s:%d\n", inet_ntoa(src), id->src_port, inet_ntoa(dst), id->dst_port);
  /* NOTE: we accept everything right now, did we want a port? */
  
  //struct pp_conn *conn = mempool_alloc(&pp_conn_pool);

  unsigned long addr = id->src_ip; // ixev returns this host as src and remote as dst
  unsigned short port = id->src_port;
  uv_tcp_t* listening_socket = ixuv__get_binding(addr, port);
  if (listening_socket==NULL)
  {
    return NULL; //No listening socket: do not accept connections.
  }


  ixev_ctx* ctx = (ixev_ctx*) malloc(sizeof(ixev_ctx)); // @TODO: Implement mempool allocation
  if (!ctx)
    return NULL;

  ixev_ctx_init(ctx);
  ctx->user_data = 0;
  ixev_set_handler(ctx, IXEVIN|IXEVOUT, &ixuv__multiplex_handler); //Handles both in and out events, to avoid constant reregistration.
  ixuv__enqueue_ctx(listening_socket, ctx);

  if(listening_socket->connection_cb != NULL){ //Maybe while pending_ctx_head != NULL, but would go into infinite loop if user supplies method that does not poll queue. (Calls accept)
    listening_socket->connection_cb((uv_stream_t*)listening_socket, 0); //unsure what errors could have occured, also right way to have this?
  }


  return ctx;
}

int uv__tcp_accept(uv_tcp_t* server, uv_tcp_t* client){
  ixev_ctx* ctx = ixuv__dequeue_ctx(server);
  if (ctx==NULL){
    return -EAGAIN;
  }
  client->_ixev_ctx = ctx;
  ctx->user_data = (unsigned long) client;
  return 0;
}





static int maybe_new_socket(uv_tcp_t* handle, int domain, int flags) {
  int sockfd;
  int err;

  if (uv__stream_fd(handle) != -1)
    return 0;

  err = uv__socket(domain, SOCK_STREAM, 0);
  if (err < 0)
    return err;
  sockfd = err;

  err = uv__stream_open((uv_stream_t*) handle, sockfd, flags);
  if (err) {
    uv__close(sockfd);
    return err;
  }

  return 0;
}


int uv__tcp_bind(uv_tcp_t* tcp,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 unsigned int flags) {
  struct sockaddr_in* in_addr = (struct sockaddr_in*) addr;
  ixuv__put_binding(in_addr->sin_addr.s_addr, in_addr->sin_port, tcp);
  return 0;

/*  int err;
  int on;
*/
  /* Cannot set IPv6-only mode on non-IPv6 socket. */
 /* if ((flags & UV_TCP_IPV6ONLY) && addr->sa_family != AF_INET6)
    return -EINVAL;

  err = maybe_new_socket(tcp,
                         addr->sa_family,
                         UV_STREAM_READABLE | UV_STREAM_WRITABLE);
  if (err)
    return err;

  on = 1;
  if (setsockopt(tcp->io_watcher.fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)))
    return -errno;

#ifdef IPV6_V6ONLY
  if (addr->sa_family == AF_INET6) {
    on = (flags & UV_TCP_IPV6ONLY) != 0;
    if (setsockopt(tcp->io_watcher.fd,
                   IPPROTO_IPV6,
                   IPV6_V6ONLY,
                   &on,
                   sizeof on) == -1) {
      return -errno;
    }
  }
#endif

  errno = 0;
  if (bind(tcp->io_watcher.fd, addr, addrlen) && errno != EADDRINUSE)
    return -errno;
  tcp->delayed_error = -errno;

  if (addr->sa_family == AF_INET6)
    tcp->flags |= UV_HANDLE_IPV6;

  return 0;*/
}


struct ixev_conn_ops pp_conn_ops = {
  .accept   = NULL,
  .release  = NULL,
};


int uv__tcp_connect(uv_connect_t* req,
                    uv_tcp_t* handle,
                    const struct sockaddr* addr,
                    unsigned int addrlen,
                    uv_connect_cb cb) {
  int err;
  int r;

 r = ixev_init(&pp_conn_ops);
 

  assert(handle->type == UV_TCP);

  if (handle->connect_req != NULL)
    return -EALREADY;  /* FIXME(bnoordhuis) -EINVAL or maybe -EBUSY. */

  err = maybe_new_socket(handle,
                         addr->sa_family,
                         UV_STREAM_READABLE | UV_STREAM_WRITABLE);
  if (err)
    return err;

  handle->delayed_error = 0;

  do
    r = connect(uv__stream_fd(handle), addr, addrlen);
  while (r == -1 && errno == EINTR);

  if (r == -1) {
    if (errno == EINPROGRESS)
      ; /* not an error */
    else if (errno == ECONNREFUSED)
    /* If we get a ECONNREFUSED wait until the next tick to report the
     * error. Solaris wants to report immediately--other unixes want to
     * wait.
     */
      handle->delayed_error = -errno;
    else
      return -errno;
  }

  uv__req_init(handle->loop, req, UV_CONNECT);
  req->cb = cb;
  req->handle = (uv_stream_t*) handle;
  QUEUE_INIT(&req->queue);
  handle->connect_req = req;

  uv__io_start(handle->loop, &handle->io_watcher, UV__POLLOUT);

  if (handle->delayed_error)
    uv__io_feed(handle->loop, &handle->io_watcher);

  return 0;
}


int uv_tcp_open(uv_tcp_t* handle, uv_os_sock_t sock) {
  int err;

  err = uv__nonblock(sock, 1);
  if (err)
    return err;
  
  return uv__stream_open((uv_stream_t*)handle,
                         sock,
                         UV_STREAM_READABLE | UV_STREAM_WRITABLE);
}


int uv_tcp_getsockname(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {
  socklen_t socklen;

  if (handle->delayed_error)
    return handle->delayed_error;

  if (uv__stream_fd(handle) < 0)
    return -EINVAL;  /* FIXME(bnoordhuis) -EBADF */

  /* sizeof(socklen_t) != sizeof(int) on some systems. */
  socklen = (socklen_t) *namelen;

  if (getsockname(uv__stream_fd(handle), name, &socklen))
    return -errno;

  *namelen = (int) socklen;
  return 0;
}


int uv_tcp_getpeername(const uv_tcp_t* handle,
                       struct sockaddr* name,
                       int* namelen) {
  socklen_t socklen;

  if (handle->delayed_error)
    return handle->delayed_error;

  if (uv__stream_fd(handle) < 0)
    return -EINVAL;  /* FIXME(bnoordhuis) -EBADF */

  /* sizeof(socklen_t) != sizeof(int) on some systems. */
  socklen = (socklen_t) *namelen;

  if (getpeername(uv__stream_fd(handle), name, &socklen))
    return -errno;

  *namelen = (int) socklen;
  return 0;
}


int uv_tcp_listen(uv_tcp_t* tcp, int backlog, uv_connection_cb cb) {
  static int single_accept = -1;
  int err;

  fprintf(stderr, "Listening to  NEW CONNECTIONs. LOL :(\n");
  fflush(stderr);
  if (tcp->delayed_error)
    return tcp->delayed_error;

  if (single_accept == -1) {
    const char* val = getenv("UV_TCP_SINGLE_ACCEPT");
    single_accept = (val != NULL && atoi(val) != 0);  /* Off by default. */
  }

  if (single_accept)
    tcp->flags |= UV_TCP_SINGLE_ACCEPT;

  // err = maybe_new_socket(tcp, AF_INET, UV_STREAM_READABLE);
  if (err)
    return err;

  // if (listen(tcp->io_watcher.fd, backlog))
  //   return -errno;

  tcp->connection_cb = cb; // Use this from special callback given to IXEV

  /* Start listening for connections. */
  // tcp->io_watcher.cb = uv__server_io;
  // uv__io_start(tcp->loop, &tcp->io_watcher, UV__POLLIN);

  return 0;
}


int uv__tcp_nodelay(int fd, int on) {
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)))
    return -errno;
  return 0;
}


int uv__tcp_keepalive(int fd, int on, unsigned int delay) {
  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)))
    return -errno;

#ifdef TCP_KEEPIDLE
  if (on && setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &delay, sizeof(delay)))
    return -errno;
#endif

  /* Solaris/SmartOS, if you don't support keep-alive,
   * then don't advertise it in your system headers...
   */
  /* FIXME(bnoordhuis) That's possibly because sizeof(delay) should be 1. */
#if defined(TCP_KEEPALIVE) && !defined(__sun)
  if (on && setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &delay, sizeof(delay)))
    return -errno;
#endif

  return 0;
}


int uv_tcp_nodelay(uv_tcp_t* handle, int on) {
  int err;

  if (uv__stream_fd(handle) != -1) {
    err = uv__tcp_nodelay(uv__stream_fd(handle), on);
    if (err)
      return err;
  }

  if (on)
    handle->flags |= UV_TCP_NODELAY;
  else
    handle->flags &= ~UV_TCP_NODELAY;

  return 0;
}


int uv_tcp_keepalive(uv_tcp_t* handle, int on, unsigned int delay) {
  int err;

  if (uv__stream_fd(handle) != -1) {
    err =uv__tcp_keepalive(uv__stream_fd(handle), on, delay);
    if (err)
      return err;
  }

  if (on)
    handle->flags |= UV_TCP_KEEPALIVE;
  else
    handle->flags &= ~UV_TCP_KEEPALIVE;

  /* TODO Store delay if uv__stream_fd(handle) == -1 but don't want to enlarge
   *      uv_tcp_t with an int that's almost never used...
   */

  return 0;
}


int uv_tcp_simultaneous_accepts(uv_tcp_t* handle, int enable) {
  if (enable)
    handle->flags &= ~UV_TCP_SINGLE_ACCEPT;
  else
    handle->flags |= UV_TCP_SINGLE_ACCEPT;
  return 0;
}


void uv__tcp_close(uv_tcp_t* handle) {
  uv__stream_close((uv_stream_t*)handle);
}
