/* Copyright (c) 2013, Ben Noordhuis <info@bnoordhuis.nl>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef UV_SRC_HEAP_H_
#define UV_SRC_HEAP_H_

#include <stddef.h>  /* NULL */

#if defined(__GNUC__)
# define HEAP_EXPORT(declaration) __attribute__((unused)) static declaration
#else
# define HEAP_EXPORT(declaration) static declaration
#endif

struct heap_node {
  struct heap_node* left;
  struct heap_node* right;
  struct heap_node* parent;
};

/* A binary min heap.  The usual properties hold: the root is the lowest
 * element in the set, the height of the tree is at most log2(nodes) and
 * it's always a complete binary tree.
 *
 * The heap function try hard to detect corrupted tree nodes at the cost
 * of a minor reduction in performance.  Compile with -DNDEBUG to disable.
 */
// 可以先通过 [Binary Tree](https://www.geeksforgeeks.org/binary-tree-set-3-types-of-binary-tree/) 回顾
// binary tree 的知识
//
// 使用 min heap，是利用该结构以下的特性：
// - 根节点总是极值（最小 or 最大，根据需求）
// - 相比数组，插入和移除的效率更高
// 
// 上面的特性刚好符合 libuv 处理 timer 的需求，因此选择该结构
struct heap {
  struct heap_node* min;
  unsigned int nelts;
};

/* Return non-zero if a < b. */
typedef int (*heap_compare_fn)(const struct heap_node* a,
                               const struct heap_node* b);

/* Public functions. */
HEAP_EXPORT(void heap_init(struct heap* heap));
HEAP_EXPORT(struct heap_node* heap_min(const struct heap* heap));
HEAP_EXPORT(void heap_insert(struct heap* heap,
                             struct heap_node* newnode,
                             heap_compare_fn less_than));
HEAP_EXPORT(void heap_remove(struct heap* heap,
                             struct heap_node* node,
                             heap_compare_fn less_than));
HEAP_EXPORT(void heap_dequeue(struct heap* heap, heap_compare_fn less_than));

/* Implementation follows. */

HEAP_EXPORT(void heap_init(struct heap* heap)) {
  heap->min = NULL;
  heap->nelts = 0;
}

HEAP_EXPORT(struct heap_node* heap_min(const struct heap* heap)) {
  return heap->min;
}

/* Swap parent with child. Child moves closer to the root, parent moves away. */
static void heap_node_swap(struct heap* heap,
                           struct heap_node* parent,
                           struct heap_node* child) {
  struct heap_node* sibling;
  struct heap_node t;

  t = *parent;
  *parent = *child;
  *child = t;

  parent->parent = child;
  if (child->left == child) {
    child->left = parent;
    sibling = child->right;
  } else {
    child->right = parent;
    sibling = child->left;
  }
  if (sibling != NULL)
    sibling->parent = child;

  if (parent->left != NULL)
    parent->left->parent = parent;
  if (parent->right != NULL)
    parent->right->parent = parent;

  if (child->parent == NULL)
    heap->min = child;
  else if (child->parent->left == parent)
    child->parent->left = child;
  else
    child->parent->right = child;
}

HEAP_EXPORT(void heap_insert(struct heap* heap,
                             struct heap_node* newnode,
                             heap_compare_fn less_than)) {
  struct heap_node** parent;
  struct heap_node** child;
  unsigned int path;
  unsigned int n;
  unsigned int k;

  newnode->left = NULL;
  newnode->right = NULL;
  newnode->parent = NULL;

  /* Calculate the path from the root to the insertion point.  This is a min
   * heap so we always insert at the left-most free node of the bottom row.
   */
  path = 0;
  // 计算目标叶子节点到根节点的路径
  //
  // heap->nelts 表示堆中的节点数，nelts 表示 number of elements 
  //
  // 因为是 [complete binary tree](https://www.geeksforgeeks.org/binary-tree-set-3-types-of-binary-tree/) 的缘故，
  // 节点最终只会落在 Left 或者 Right 两个位置。假设使用 0 表示 Left，使用 1 表示 Right，且跟节点从 1 开始，
  // 那么第偶数个节点，必定在 Left 位置，第基数个节点必定在 Right 位置
  //
  // 下面的代码，就是先通过目标插入点的奇偶性，计算其落在 Left 还是 Right，然后通过不断 `n /= 2` 找出父节点，并计算其落点
  // 一直上推到第 2 层 `n >= 2`，计算出从根节点到目标插入点的路径（比如 01010）以及路径的步数 k
  // 另外，路径存按从下到上的顺序，存放在整型 `path` 的从右到左的 bit 位中
  for (k = 0, n = 1 + heap->nelts; n >= 2; k += 1, n /= 2)
    path = (path << 1) | (n & 1);

  /* Now traverse the heap using the path we calculated in the previous step. */
  parent = child = &heap->min;
  while (k > 0) {
    parent = child;
    if (path & 1)
      child = &(*child)->right;
    else
      child = &(*child)->left;
    path >>= 1;
    k -= 1;
  }

  /* Insert the new node. */
  newnode->parent = *parent;
  *child = newnode;
  heap->nelts += 1;

  /* Walk up the tree and check at each node if the heap property holds.
   * It's a min heap so parent < child must be true.
   */
  // 插入后要重新向上冒泡，保证是 min heap 的设定
  while (newnode->parent != NULL && less_than(newnode, newnode->parent))
    heap_node_swap(heap, newnode->parent, newnode);
}

HEAP_EXPORT(void heap_remove(struct heap* heap,
                             struct heap_node* node,
                             heap_compare_fn less_than)) {
  struct heap_node* smallest;
  struct heap_node** max;
  struct heap_node* child;
  unsigned int path;
  unsigned int k;
  unsigned int n;

  if (heap->nelts == 0)
    return;

  /* Calculate the path from the min (the root) to the max, the left-most node
   * of the bottom row.
   */
  path = 0;
  for (k = 0, n = heap->nelts; n >= 2; k += 1, n /= 2)
    path = (path << 1) | (n & 1);

  /* Now traverse the heap using the path we calculated in the previous step. */
  // 移除时，先找到最深一层的最右边一个节点，然后将该节点和要移除的节点交换位置
  // 这样可以避免重排整个堆的顺序
  max = &heap->min;
  while (k > 0) {
    if (path & 1)
      max = &(*max)->right;
    else
      max = &(*max)->left;
    path >>= 1;
    k -= 1;
  }

  heap->nelts -= 1;

  /* Unlink the max node. */
  child = *max;
  *max = NULL;

  if (child == node) {
    /* We're removing either the max or the last node in the tree. */
    if (child == heap->min) {
      heap->min = NULL;
    }
    return;
  }

  /* Replace the to be deleted node with the max node. */
  child->left = node->left;
  child->right = node->right;
  child->parent = node->parent;

  // 重置要移除的节点的子孙节点的引用
  if (child->left != NULL) {
    child->left->parent = child;
  }

  if (child->right != NULL) {
    child->right->parent = child;
  }

  // 重置要移除节点的父节点对该节点的引用
  if (node->parent == NULL) {
    heap->min = child;
  } else if (node->parent->left == node) {
    node->parent->left = child;
  } else {
    node->parent->right = child;
  }

  /* Walk down the subtree and check at each node if the heap property holds.
   * It's a min heap so parent < child must be true.  If the parent is bigger,
   * swap it with the smallest child.
   */
  // 因为是将最深一层的最右边一个节点交换来的，所以该节点可以比较大，先处理
  // 比较大的情况，保证整个堆是 min heap
  for (;;) {
    smallest = child;
    if (child->left != NULL && less_than(child->left, smallest))
      smallest = child->left;
    if (child->right != NULL && less_than(child->right, smallest))
      smallest = child->right;
    if (smallest == child)
      break;
    heap_node_swap(heap, child, smallest);
  }

  /* Walk up the subtree and check that each parent is less than the node
   * this is required, because `max` node is not guaranteed to be the
   * actual maximum in tree
   */
  // 接上面的步骤，再处理交换来的节点可能比较小的情况，最终保证移除节点后，整个堆
  // 依然符合 min heap 的设定
  while (child->parent != NULL && less_than(child, child->parent))
    heap_node_swap(heap, child->parent, child);
}

HEAP_EXPORT(void heap_dequeue(struct heap* heap, heap_compare_fn less_than)) {
  heap_remove(heap, heap->min, less_than);
}

#undef HEAP_EXPORT

#endif  /* UV_SRC_HEAP_H_ */
