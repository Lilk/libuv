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
#include "tree.h"
#include "internal.h"
#include "heap-inl.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ixev.h"

 uv_buf_t null_buf = {
  .base = NULL,
  .len = 0,
 };


static void ixuv_release(struct ixev_ctx *ctx)
{
  printf("Need to implement a nice release method: %p (%p)\n", ctx, ctx->user_data);
  // struct pp_conn *conn = container_of(ctx, struct pp_conn, ctx);
  uv_tcp_t* stream = (uv_tcp_t*) ctx->user_data; 
  ctx->user_data = (int64_t) NULL;
  if(stream != NULL && stream->read_cb != NULL){
    // printf("Gonna call read callback with nullbuf\n");
    stream->read_cb((uv_stream_t*)stream, UV_EOF, &null_buf);
    stream->flags |= UV_CLOSING;
    uv__make_close_pending((uv_handle_t*) stream); //Since we return immediately from uv close in case of TCP to allow IX to close the connection before calling the callback
    //TODO;  CHECK if node will call uv_close upon receiving UV_EOF on reab_cb
    // printf("Call OK\n");
  }
  if(stream != NULL){
    // printf("Nulling context and fd\n");
    stream-> _ixev_ctx = NULL;
    stream->io_watcher.fd = -1;
  }
  // printf("Freeing ctx\n");
  free(ctx);
  // printf("Freed context\n");
  // mempool_free(&pp_conn_pool, conn);
}

void ixuv__handle_uds_events(uv_loop_t* loop, uint32_t events, int fd );

static __thread uv_loop_t* ixuv__static_loop = NULL;

void ixuv__uds_handler(uint32_t events, int fd ) {
  ixuv__handle_uds_events(ixuv__static_loop, events, fd);
}


struct ixev_conn_ops ixuv_conn_ops = {
  .accept   = &ixuv__accept,
  .release  = &ixuv_release,
  .uds_handler = &ixuv__uds_handler,
};




int ixuv__init(uv_loop_t* loop){

  if ( ixuv__static_loop == NULL ) {
    ixuv__static_loop = loop;
    ixev_init(&ixuv_conn_ops);
 

    int ret;

    ret = ixev_init_thread();
    if (ret) {
      printf("unable to init IXUV\n");
      return -1;
    };
   }

}

int uv_loop_init(uv_loop_t* loop) {
  int err;

  err = ixuv__init(loop);
  if(err){
    return;
  }


  uv__signal_global_once_init();

  memset(loop, 0, sizeof(*loop));
  heap_init((struct heap*) &loop->timer_heap);
  QUEUE_INIT(&loop->wq);
  QUEUE_INIT(&loop->active_reqs);
  QUEUE_INIT(&loop->idle_handles);
  QUEUE_INIT(&loop->async_handles);
  QUEUE_INIT(&loop->check_handles);
  QUEUE_INIT(&loop->prepare_handles);
  QUEUE_INIT(&loop->handle_queue);

  loop->nfds = 0;
  loop->watchers = NULL;
  loop->nwatchers = 0;
  QUEUE_INIT(&loop->pending_queue);
  QUEUE_INIT(&loop->watcher_queue);

  loop->closing_handles = NULL;
  uv__update_time(loop);
  uv__async_init(&loop->async_watcher);
  loop->signal_pipefd[0] = -1;
  loop->signal_pipefd[1] = -1;
  loop->backend_fd = -1;
  loop->emfile_fd = -1;

  loop->timer_counter = 0;
  loop->stop_flag = 0;

  err = uv__platform_loop_init(loop);
  if (err)
    return err;

  uv_signal_init(loop, &loop->child_watcher);
  uv__handle_unref(&loop->child_watcher);
  loop->child_watcher.flags |= UV__HANDLE_INTERNAL;
  QUEUE_INIT(&loop->process_handles);

  if (uv_rwlock_init(&loop->cloexec_lock))
    abort();

  if (uv_mutex_init(&loop->wq_mutex))
    abort();

  if (uv_async_init(loop, &loop->wq_async, uv__work_done))
    abort();

  uv__handle_unref(&loop->wq_async);
  loop->wq_async.flags |= UV__HANDLE_INTERNAL;

  return 0;
}


void uv__loop_close(uv_loop_t* loop) {
  uv__signal_loop_cleanup(loop);
  uv__platform_loop_delete(loop);
  uv__async_stop(loop, &loop->async_watcher);

  if (loop->emfile_fd != -1) {
    uv__close(loop->emfile_fd);
    loop->emfile_fd = -1;
  }

  if (loop->backend_fd != -1) {
    uv__close(loop->backend_fd);
    loop->backend_fd = -1;
  }

  uv_mutex_lock(&loop->wq_mutex);
  assert(QUEUE_EMPTY(&loop->wq) && "thread pool work queue not empty!");
  assert(!uv__has_active_reqs(loop));
  uv_mutex_unlock(&loop->wq_mutex);
  uv_mutex_destroy(&loop->wq_mutex);

  /*
   * Note that all thread pool stuff is finished at this point and
   * it is safe to just destroy rw lock
   */
  uv_rwlock_destroy(&loop->cloexec_lock);

#if 0
  assert(QUEUE_EMPTY(&loop->pending_queue));
  assert(QUEUE_EMPTY(&loop->watcher_queue));
  assert(loop->nfds == 0);
#endif

  free(loop->watchers);
  loop->watchers = NULL;
  loop->nwatchers = 0;
}


int uv__loop_configure(uv_loop_t* loop, uv_loop_option option, va_list ap) {
  if (option != UV_LOOP_BLOCK_SIGNAL)
    return UV_ENOSYS;

  if (va_arg(ap, int) != SIGPROF)
    return UV_EINVAL;

  loop->flags |= UV_LOOP_BLOCK_SIGPROF;
  return 0;
}
