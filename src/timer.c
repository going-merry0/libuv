/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
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
#include "uv-common.h"
#include "heap-inl.h"

#include <assert.h>
#include <limits.h>


static struct heap *timer_heap(const uv_loop_t* loop) {
#ifdef _WIN32
  return (struct heap*) loop->timer_heap;
#else
  return (struct heap*) &loop->timer_heap;
#endif
}


static int timer_less_than(const struct heap_node* ha,
                           const struct heap_node* hb) {
  const uv_timer_t* a;
  const uv_timer_t* b;

  a = container_of(ha, uv_timer_t, heap_node);
  b = container_of(hb, uv_timer_t, heap_node);

  if (a->timeout < b->timeout)
    return 1;
  if (b->timeout < a->timeout)
    return 0;

  /* Compare start_id when both have the same timeout. start_id is
   * allocated with loop->timer_counter in uv_timer_start().
   */
  return a->start_id < b->start_id;
}


int uv_timer_init(uv_loop_t* loop, uv_timer_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_TIMER);
  handle->timer_cb = NULL;
  handle->timeout = 0;
  handle->repeat = 0;
  return 0;
}


int uv_timer_start(uv_timer_t* handle,
                   uv_timer_cb cb,
                   uint64_t timeout,
                   uint64_t repeat) {
  uint64_t clamped_timeout;

  // loop->time 表示 loop 当前的时间。loop 每次迭代开始时，会用当次时间更新该值
  // clamped_timeout 就是该 timer 未来超时的时间点，这里直接计算好，这样未来就不需要
  // 计算了，直接从 timers 中取符合条件的即可
  if (clamped_timeout < timeout)
    clamped_timeout = (uint64_t) -1;

  handle->timer_cb = cb;
  handle->timeout = clamped_timeout;
  handle->repeat = repeat;
  
  // 除了预先计算好的 clamped_timeout 以外，未来当 clamped_timeout 相同时，使用这里的
  // 自增 start_id 作为比较条件来觉得 handle 的执行先后顺序
  handle->start_id = handle->loop->timer_counter++;

  // 将 handle 插入到 timer_heap 中，这里的 heap 是 binary min heap，所以根节点就是
  // clamped_timeout 值（或者 start_id）最小的 handle
  heap_insert(timer_heap(handle->loop),
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  // 设置 handle 的开始状态
  uv__handle_start(handle);

  return 0;
}


int uv_timer_stop(uv_timer_t* handle) {
  if (!uv__is_active(handle))
    return 0;

  // 将 handle 移出 timer_heap，和 heap_insert 操作一样，除了移出之外
  // 还会维护 timer_heap 以保障其始终是 binary min heap
  heap_remove(timer_heap(handle->loop),
              (struct heap_node*) &handle->heap_node,
              timer_less_than);
  // 设置 handle 的状态为停止
  uv__handle_stop(handle);

  return 0;
}


int uv_timer_again(uv_timer_t* handle) {
  if (handle->timer_cb == NULL)
    return UV_EINVAL;

  if (handle->repeat) {
    uv_timer_stop(handle);
    uv_timer_start(handle, handle->timer_cb, handle->repeat, handle->repeat);
  }

  return 0;
}


void uv_timer_set_repeat(uv_timer_t* handle, uint64_t repeat) {
  handle->repeat = repeat;
}


uint64_t uv_timer_get_repeat(const uv_timer_t* handle) {
  return handle->repeat;
}


uint64_t uv_timer_get_due_in(const uv_timer_t* handle) {
  if (handle->loop->time >= handle->timeout)
    return 0;

  return handle->timeout - handle->loop->time;
}


int uv__next_timeout(const uv_loop_t* loop) {
  const struct heap_node* heap_node;
  const uv_timer_t* handle;
  uint64_t diff;

  heap_node = heap_min(timer_heap(loop));
  if (heap_node == NULL)
    return -1; /* block indefinitely */

  handle = container_of(heap_node, uv_timer_t, heap_node);
  if (handle->timeout <= loop->time)
    return 0;

  diff = handle->timeout - loop->time;
  if (diff > INT_MAX)
    diff = INT_MAX;

  return (int) diff;
}


void uv__run_timers(uv_loop_t* loop) {
  struct heap_node* heap_node;
  uv_timer_t* handle;

  for (;;) {
    // 取根节点，该值保证始终是所有待执行的 handle
    // 中，最先超时的那一个
    heap_node = heap_min(timer_heap(loop));
    if (heap_node == NULL)
      break;

    handle = container_of(heap_node, uv_timer_t, heap_node);
    if (handle->timeout > loop->time)
      break;

    // 停止、移出 handle、顺便维护 timer_heap
    uv_timer_stop(handle);
    // 如果是需要 repeat 的 handle，则重新加入到 timer_heap 中
    // 会在下一次事件循环中、由本方法继续执行
    uv_timer_again(handle);
    // 执行超时 handle 其对应的回调
    handle->timer_cb(handle);
  }
}


void uv__timer_close(uv_timer_t* handle) {
  uv_timer_stop(handle);
}
