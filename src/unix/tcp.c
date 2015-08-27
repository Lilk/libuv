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


 #include <netinet/in.h>

 #include <mempool.h>



int uv_tcp_init(uv_loop_t* loop, uv_tcp_t* tcp) {
  uv__stream_init(loop, (uv_stream_t*)tcp, UV_TCP);
  tcp->_ixev_ctx = NULL;
  tcp->pending_ctx_head = NULL;
  return 0;
}

static uv_tcp_t* singleton_listening_socket = NULL;
static unsigned long singleton_listening_socket_addr = NULL;
static unsigned short singleton_listening_socket_port = NULL;


/*io handler to deal with connection acceptance for tcp */
void ixuv__tcp_server_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  

  uv_stream_t* stream = container_of(w, uv_stream_t, io_watcher);

  void * addr = ( (uv_tcp_t*)stream)->pending_ctx_head == NULL? NULL: (void*) ( (uv_tcp_t*)stream)->pending_ctx_head->_ixev_ctx;
  fprintf(stderr, " - Server %p is calling its connection_cb, preparing to let user accept %p\n", stream, addr);
  stream->connection_cb(stream, 0);
  fprintf(stderr, " - CONNECTION CB DONE\n");
}


/*
 * Puts a binding from an address to a server handle.
 */
void ixuv__put_binding(unsigned long addr, unsigned short port, uv_tcp_t* tcp){
  printf("Put %ld %d: %p\n", addr, port, tcp);
  singleton_listening_socket_addr = addr;
  singleton_listening_socket_port = port;
  singleton_listening_socket = tcp;
}
/*
 * Gets the server handle for an address.
 */
uv_tcp_t* ixuv__get_binding(unsigned long addr, unsigned short port){
  // if( addr != singleton_listening_socket_addr ||  port != singleton_listening_socket_port) return NULL;
  return singleton_listening_socket;
}


/* Enqueue an incoming ixev_ctx to a server handle */
void ixuv__enqueue_ctx(uv_tcp_t* tcp, ixev_ctx* ctx){
  ctx_post* post = (ctx_post*) malloc(sizeof(ctx_post));
  post->_ixev_ctx = ctx;
  if(tcp->pending_ctx_head == NULL){
    tcp->pending_ctx_head = post;
    tcp->pending_ctx_tail = post;
  }
  tcp->pending_ctx_tail->next = post;
  tcp->pending_ctx_tail = post;
  post->next = NULL;

}
/* Dequeue an incoming ixev_ctx from a server handle */
ixev_ctx* ixuv__dequeue_ctx(uv_tcp_t* tcp){
  if(tcp->pending_ctx_head == NULL){
    return NULL;
  }
  ctx_post* post = tcp->pending_ctx_head;
  ixev_ctx* ixev_ctx_ = post -> _ixev_ctx; 

  tcp->pending_ctx_head = post->next; 

  free(post);
  return ixev_ctx_;
}


void ixuv_release(struct ixev_ctx *ctx);

static void ixuv__multiplex_handler(struct ixev_ctx *ctx, unsigned int reason) {
    // printf("ixuv__multiplex_handler, reason: %d (IXEVIN: %d, IXEVOUT: %d, IXEVHUP %d)\n", reason, IXEVIN, IXEVOUT,IXEVHUP);
    uv_tcp_t* stream = (uv_tcp_t*) ctx->user_data;
    if(stream==NULL){
      fprintf(stderr, "Null context from ctx handler - in need of a queue?\n");
      return;
    }
    if(reason == IXEVIN && stream->read_cb != NULL && stream->flags & UV_STREAM_READING){
        uv_buf_t buf;
        stream->alloc_cb((uv_handle_t*)stream, 64 * 1024, &buf);
        int read = ixev_recv(ctx, buf.base, buf.len);
        printf("READ (%d): [%s]\n\n CALLING READ CB \n\n", read, buf.base);
       
        stream->read_cb((uv_stream_t*)stream, read, &buf);     
        printf("READ CB EXeCUteD\n");
    } if (  reason == IXEVHUP){
        // fprintf(stderr, "connection died\n");
        ((uv_tcp_t*) ctx->user_data)-> _ixev_ctx = NULL; // Prevent us from trying to close the ixev_ctx if it was already close by IX. 
        ixuv_release(ctx);
        /* Possible place to store state in connection that we lost the connection, to optimise shutdown.
            Note that int ixev_event_mask = IXEVIN; // IXEVIN|IXEVOUT|IXEVHUP in 
            struct ixev_ctx *ixuv__accept(struct ip_tuple *id) needs to be set to forward such events.
         */
    } if ( reason == IXEVOUT){
        /* Here would be the place to call the write callback if 
            the callback should be called after IX processing rather
            than after the uv layer has passed the callback to IX.  */
    }
    // printf("ixuv__multiplex_handler finished\n");
}

// void call_connection_cbs(){
//   uv_tcp_t* listening_socket = ixuv__get_binding(addr, port);
//   if (listening_socket==NULL)
//   {
//     fprintf(stderr, "ixuv__accept: listening_server null\n");
//     return NULL; //No listening socket: do not accept connections.
//   }
//   listening_socket->connection_cb((uv_stream_t*)listening_socket, 0);
// }


struct ixev_ctx *ixuv__accept(struct ip_tuple *id)
{

  struct in_addr src, dst;
  src.s_addr = id->src_ip;
  dst.s_addr = id->dst_ip;
  

  unsigned long addr = id->src_ip; // ixev returns this host as src and remote as dst
  unsigned short port = id->src_port;

  // printf("looking up %ld %d\n", addr, port);

  uv_tcp_t* listening_socket = ixuv__get_binding(addr, port);
  if (listening_socket==NULL)
  {
    fprintf(stderr, "ixuv__accept: listening_server null\n");
    return NULL; //No listening socket: do not accept connections.
  }
   if(listening_socket->connection_cb == NULL){ //Maybe while pending_ctx_head != NULL, but would go into infinite loop if user supplies method that does not poll queue. (Calls accept)
    fprintf(stderr, "ixuv__accept: Bound listening server has no connection callback\n");
    return NULL;
  }

  ixev_ctx* ctx = (ixev_ctx*) malloc(sizeof(ixev_ctx)); // @TODO: Implement mempool allocation
  if (!ctx){
    printf("Returning NULL\n");
    return NULL;
  }

  ixev_ctx_init(ctx);
  ctx->user_data = 0;
  int ixev_event_mask = IXEVIN|IXEVHUP; // IXEVIN|IXEVOUT|IXEVHUP to get all callbacks to static void ixuv__multiplex_handler(struct ixev_ctx *ctx, unsigned int reason)
  ixev_set_handler(ctx, ixev_event_mask, &ixuv__multiplex_handler); //Handles both in and out events, to avoid constant reregistration.
  ixuv__enqueue_ctx(listening_socket, ctx);
  

  uv__io_feed(listening_socket->loop, &listening_socket->io_watcher);


  // listening_socket->connection_cb((uv_stream_t*)listening_socket, 0); //unsure what errors could have occured

  printf("Returning %p ixev_context, calling ix_accept\n", ctx);
  return ctx;



}

int uv__tcp_accept(uv_tcp_t* server, uv_tcp_t* client){
  // printf("Calling uv__tcp_accept\n");
  ixev_ctx* ctx = ixuv__dequeue_ctx(server);
  if (ctx==NULL){

    return -EAGAIN;
  }
  client->_ixev_ctx = ctx;
  client->io_watcher.fd = ctx; //SUPER HACK TO AVOID asserts, be aware, here be dragons!!
  ctx->user_data = (unsigned long) client;
  // printf("Putting uv ctx %p as user_data for ctx %p\n", client, ctx);
  return 0;
}

void uv__write_req_finish(uv_write_t* req);


struct ixuv_ref {
  struct ixev_ref _ixev_ref;
  uv_tcp_t* _stream;
  uv_write_t* _req;
  int _wq_diff;
};


static struct mempool_datastore ixuv__ref_datastore;
static __thread struct mempool ixuv__ref_pool;



static void ixuv__write_complete_callback(struct ixev_ref *ixev_ref_ptr){
  printf("Write completed\n");
   struct ixuv_ref* ref = container_of(ixev_ref_ptr, struct ixuv_ref, _ixev_ref);

   printf("A\n");
   uv_tcp_t* stream = ref->_stream;
   uv_write_t* req = ref->_req;

   printf("B\n");

   stream->write_queue_size -= ref->_wq_diff;
      printf("C\n");

   // stream->write_queue_size -= 1;
   // QUEUE_INSERT_TAIL(&stream->write_completed_queue, &req->queue);
   // uv__io_feed(stream->loop, &stream->io_watcher);
      printf("D\n");

   uv__write_req_finish(req);
   // printf("Write completed\n");
   // free(ixev_ref_ptr);
      printf("E\n");

    mempool_free(&ixuv__ref_pool, ref);

    printf("freed request\n");
}

#define ZEROCOPY



int uv__tcp_write(uv_write_t* req,
             uv_tcp_t* handle,
             const uv_buf_t bufs[],
             unsigned int nbufs,
             uv_write_cb cb){
  unsigned int i;
  uv_tcp_t* stream = handle; //alias
                             //
  fprintf(stderr, "uv__tcp write called: req; %p handle %p flow: %p bufs %d \n", req, handle, handle->_ixev_ctx, nbufs);


  for(i = 0; i < nbufs; i++){
    printf("buf %d: %p [%s]\n",i, bufs[i].base, bufs[i].base);
  }

  #ifdef ZEROCOPY
  int n_sent = 0;
  static bool initialised_mempool = false;
  if( unlikely( !initialised_mempool)){
    int ret;
      ret = mempool_create_datastore(&ixuv__ref_datastore, 16384*128, sizeof(struct ixuv_ref), 0, MEMPOOL_DEFAULT_CHUNKSIZE, "ixuv_ref");
      ret |= mempool_create(&ixuv__ref_pool, &ixuv__ref_datastore);
      if (ret) {
        printf("unable to create mempool\n");
        return ret;
      }
    initialised_mempool = true;
  }
  #endif


  int error = 0;
  
  // printf("Writing to stream: %p nbufs %d\n", handle, nbufs);
  if(handle->_ixev_ctx == NULL || handle->flags & UV_CLOSED){ //@TODO: Fix better error code for closed stream(?)
    fprintf(stderr, "uv__tcp_write: IXEV ctx null - aborting write");
    return -1; //TODO: FIX better error code
  }
  int inc = 0;
  for (i = 0; i < nbufs && error >= 0;)  //++i
  {

    #ifdef ZEROCOPY
    printf("Telling IX to send %d bytes from %p\n",bufs[i].len-inc, bufs[i].base + inc );
    error = ixev_send_zc( handle->_ixev_ctx, (void*) bufs[i].base + inc, (size_t) bufs[i].len-inc); 
    #else
    error = ixev_send( handle->_ixev_ctx, (void*) bufs[i].base + inc, (size_t) bufs[i].len-inc); 
    #endif
    if(unlikely(error < 0)) {
      // if( -error == EAGAIN) {
      //     i--;
      // }
      fprintf(stderr, "uv__tcp_write error: %s\n", strerror(-error) ); 
      if ( -error != EAGAIN){
        return -error;
      }
    } else  {
      n_sent = bufs[i].len;
      if( error + inc == bufs[i].len ){
         printf("Told IX to send %d bytes from %p\n",bufs[i].len-inc, bufs[i].base + inc );
        inc = 0;
        i++;
        continue;
      }
      fprintf(stderr, "Failed to send entire buffer.\n" );
      inc += error;
    }

  }

/** Taken from uv_write2 */
  uv__req_init(stream->loop, req, UV_WRITE);
  req->cb = cb;
  req->handle = handle;
  req->error = 0;
  if(error < 0) req->error = error;
  req->send_handle = NULL;
  QUEUE_INIT(&req->queue);


  req->bufs = req->bufsml;
  
  req->nbufs = nbufs;
  req->write_index = nbufs;


  #ifdef ZEROCOPY
    // struct ixuv_ref* ref = (struct ixuv_ref*) malloc(sizeof(struct ixuv_ref));
    struct ixuv_ref* ref =  mempool_alloc(&ixuv__ref_pool);
    ref->_ixev_ref.cb = ixuv__write_complete_callback;
    ref->_stream = stream;
    ref->_req = req;
    ref->_wq_diff = n_sent;
    ixev_add_sent_cb(handle->_ixev_ctx, &ref->_ixev_ref);

    QUEUE_INSERT_TAIL(&stream->write_queue, &req->queue);
    // QUEUE_INSERT_TAIL(&stream->write_queue, &req->queue);
    stream->write_queue_size += ref->_wq_diff; //1; //Important to not make libuv/app close the stream prematurely
  #else
    // printf("sending\n");
    QUEUE_INSERT_TAIL(&stream->write_completed_queue, &req->queue);
    uv__io_feed(stream->loop, &stream->io_watcher);
  #endif

    printf("write done\n");
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

static void extract_addr_port(const struct sockaddr* addr, unsigned long* ip_addr, unsigned short* port){
  if(addr->sa_family == AF_INET ){
    // printf("Extract IPv4\n");
    struct sockaddr_in* in_addr = (struct sockaddr_in*) addr;
    *ip_addr = in_addr->sin_addr.s_addr;
    *port    = in_addr->sin_port;
    return;
  }
  if(addr->sa_family == AF_INET6){
    // printf("Extract IPv6\n");

    struct sockaddr_in6* in_addr = (struct sockaddr_in6*) addr;
    *ip_addr =  -1;
    *port    = in_addr->sin6_port;
    return;
  }
  perror("Address parsing failed\n");
}


int uv__tcp_bind(uv_tcp_t* tcp,
                 const struct sockaddr* addr,
                 unsigned int addrlen,
                 unsigned int flags) {
  unsigned long ip;
  unsigned short port;
  extract_addr_port(addr, &ip, &port);

  // printf("Putting binding for %ld:%hu\n", ip, port);
  ixuv__put_binding(ip, port, tcp);
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

/**
 * TODO: IMPLEMENT over IX when IX supports  outgoing connections
 */
int uv__tcp_connect(uv_connect_t* req,
                    uv_tcp_t* handle,
                    const struct sockaddr* addr,
                    unsigned int addrlen,
                    uv_connect_cb cb) {
  int err;
  int r;

 

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

/*
 * Deprecated, IX does not support sockets.
 */
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

void uv__mock_server_io(uv_loop_t* loop, uv__io_t* w, unsigned int events) {
  fprintf(stderr, "Mock server io, should not be called\n");
}


int uv_tcp_listen(uv_tcp_t* tcp, int backlog, uv_connection_cb cb) {
  static int single_accept = -1;
  int err;

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
   tcp->io_watcher.cb = ixuv__tcp_server_io; //uv__server_io;
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
  printf("Closing time\n");
  // uv__stream_close((uv_stream_t*)handle);
    if( ((uv_tcp_t*) handle)->_ixev_ctx != NULL){
      printf("closing handle\n");
       ixev_close(((uv_tcp_t*) handle)->_ixev_ctx);
       // ((uv_tcp_t*) handle)->_ixev_ctx->user_data = NULL;
       ((uv_tcp_t*) handle)->_ixev_ctx = NULL;
      }
      // ix_flush();
      printf("after\n");
      handle->io_watcher.fd = -1;
      // uv__handle_stop(handle);
}
