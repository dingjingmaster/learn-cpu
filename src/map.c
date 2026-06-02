/*
 * rv32emu 可依据 MIT 许可证自由再分发。使用和再分发规则见 LICENSE 文件。
 */

/*
 * 通用红黑树 map 容器。
 *
 * map 使用侵入式节点保存 key/value，提供 O(log n) 插入、删除、查找和有序遍历。
 * 断点表、文件描述符映射等模块复用该容器。实现借鉴 jemalloc rb.h，但接口被
 * 收敛成适合 rv32emu 的小型 C 容器 API。
 */

/* 该 map 实现经过了大量改造，核心红黑树算法参考 jemalloc 的 rb.h。原始 rb.h 为
 * 本项目的小型 map 容器提供了基础和灵感，特此致谢。
 *
 * 参考：
 *   https://github.com/jemalloc/jemalloc/blob/dev/include/ \
 *   jemalloc/internal/rb.h
 */

#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "map.h"

struct map_internal {
    map_node_t *root;           /* 树根。 */
    size_t key_size, data_size; /* key/value 类型大小。 */
    size_t size;                /* 节点数量。 */
    /* key 比较函数。 */
    map_cmp_t (*comparator)(const void *, const void *);
};

/* 每个红黑树节点至少需要 1 字节存储链接信息。理论上，1 字节对象在地址空间中可
 * 支持最多 2^{sizeof(void *) * 8} 个节点；但红黑树算法保证树深度不超过
 * 2 * log2(n)，n 为节点数量。
 *
 * 插入和删除等操作使用固定大小数组记录遍历路径。RB_MAX_DEPTH 保守定义为指针
 * 大小的 16 倍，确保对任何现实规模的树都有足够空间，而不按理论最大节点数分配。
 */
#define RB_MAX_DEPTH (sizeof(void *) << 4)

/* 颜色/指针打包操作宏。 */
#define RB_COLOR_MASK 1UL
#define RB_PTR_MASK (~RB_COLOR_MASK)

typedef enum { RB_BLACK = 0, RB_RED = 1 } map_color_t;

/* 简化访问模式的辅助宏。 */
#define RB_IS_RED(node) (rb_node_get_color(node) == RB_RED)
#define RB_IS_BLACK(node) (rb_node_get_color(node) == RB_BLACK)

/* 预取提示，用于改善缓存利用率。 */
#ifdef __builtin_prefetch
#define PREFETCH_READ(addr) __builtin_prefetch((addr), 0, 1)
#define PREFETCH_WRITE(addr) __builtin_prefetch((addr), 1, 1)
#else
#define PREFETCH_READ(addr) ((void) 0)
#define PREFETCH_WRITE(addr) ((void) 0)
#endif

/* 左子节点访问器。 */
static inline map_node_t *rb_node_get_left(const map_node_t *node)
{
    return node->left;
}

static inline void rb_node_set_left(map_node_t *node, map_node_t *left)
{
    node->left = left;
}

/* 右子节点访问器，统一屏蔽颜色位。 */
static inline map_node_t *rb_node_get_right(const map_node_t *node)
{
    return (map_node_t *) (((uintptr_t) node->right_red) & ~RB_COLOR_MASK);
}

static inline void rb_node_set_right(map_node_t *node, map_node_t *right)
{
    node->right_red =
        (map_node_t *) (((uintptr_t) right) |
                        (((uintptr_t) node->right_red) & RB_COLOR_MASK));
}

/* 颜色访问器。 */
static inline map_color_t rb_node_get_color(const map_node_t *node)
{
    return ((uintptr_t) node->right_red) & RB_COLOR_MASK;
}

static inline void rb_node_set_color(map_node_t *node, map_color_t color)
{
    node->right_red =
        (map_node_t *) (((uintptr_t) node->right_red & ~RB_COLOR_MASK) | color);
}

static inline void rb_node_set_red(map_node_t *node)
{
    node->right_red = (map_node_t *) (((uintptr_t) node->right_red) | RB_RED);
}

static inline void rb_node_set_black(map_node_t *node)
{
    node->right_red =
        (map_node_t *) (((uintptr_t) node->right_red) & ~RB_COLOR_MASK);
}

/* 节点初始化。 */
static inline void rb_node_init(map_node_t *node)
{
    assert((((uintptr_t) node) & RB_COLOR_MASK) == 0); /* 已正确对齐。 */
    node->left = NULL;
    node->right_red = (map_node_t *) RB_RED; /* 带红色标记的 NULL。 */
}

/* 内部辅助宏。 */
#define rb_node_rotate_left(x_node, r_node)                      \
    do {                                                         \
        (r_node) = rb_node_get_right((x_node));                  \
        rb_node_set_right((x_node), rb_node_get_left((r_node))); \
        rb_node_set_left((r_node), (x_node));                    \
    } while (0)

#define rb_node_rotate_right(x_node, r_node)                     \
    do {                                                         \
        (r_node) = rb_node_get_left((x_node));                   \
        rb_node_set_left((x_node), rb_node_get_right((r_node))); \
        rb_node_set_right((r_node), (x_node));                   \
    } while (0)

typedef struct {
    map_node_t *node;
    map_cmp_t cmp;
} rb_path_entry_t;

static void rb_remove(map_t rb, map_node_t *node)
{
    rb_path_entry_t path[RB_MAX_DEPTH];
    rb_path_entry_t *pathp = NULL, *nodep = NULL;

    /* 遍历红黑树，找到待删除目标节点。 */
    path->node = rb->root;
    pathp = path;
    while (pathp->node) {
        map_cmp_t cmp = pathp->cmp =
            (rb->comparator)(node->key, pathp->node->key);
        if (cmp == MAP_CMP_LESS) {
            pathp[1].node = rb_node_get_left(pathp->node);
        } else {
            pathp[1].node = rb_node_get_right(pathp->node);
            if (cmp == MAP_CMP_EQUAL) {
                /* 查找后继节点，为交换做准备。 */
                pathp->cmp = MAP_CMP_GREATER;
                nodep = pathp;
                for (pathp++; pathp->node; pathp++) {
                    pathp->cmp = MAP_CMP_LESS;
                    pathp[1].node = rb_node_get_left(pathp->node);
                }
                break;
            }
        }
        pathp++;
    }
    assert(nodep && nodep->node == node);

    pathp--;
    if (pathp->node != node) {
        /* 将节点与后继节点交换。 */
        map_color_t tcolor = rb_node_get_color(pathp->node);
        rb_node_set_color(pathp->node, rb_node_get_color(node));
        rb_node_set_left(pathp->node, rb_node_get_left(node));

        /* 如果节点的后继就是其右子节点，下面代码对右子指针的处理中间状态可能不
         * 完全准确；但后继节点被剪除时会正确设置该指针，因此不会造成问题。
         */
        rb_node_set_right(pathp->node, rb_node_get_right(node));
        rb_node_set_color(node, tcolor);

        /* 被剪除叶子节点的子指针之后不会再被访问，因此不需要清成 NULL。
         */
        nodep->node = pathp->node;
        pathp->node = node;
        if (nodep == path) {
            rb->root = nodep->node;
        } else {
            if (nodep[-1].cmp == MAP_CMP_LESS)
                rb_node_set_left(nodep[-1].node, nodep->node);
            else
                rb_node_set_right(nodep[-1].node, nodep->node);
        }
    } else {
        map_node_t *left = rb_node_get_left(node);
        if (left) {
            /* 节点没有后继，但有左子节点。直接摘除该节点，同时保留左子树。
             */
            assert(RB_IS_BLACK(node));
            assert(RB_IS_RED(left));
            rb_node_set_black(left);
            if (pathp == path) {
                /* 以该节点左子节点为根的子树没有变化，现在它成为整棵树的根。
                 */
                rb->root = left;
            } else {
                if (pathp[-1].cmp == MAP_CMP_LESS)
                    rb_node_set_left(pathp[-1].node, left);
                else
                    rb_node_set_right(pathp[-1].node, left);
            }
            return;
        }
        if (pathp == path) {
            /* 树中原本只有一个节点。 */
            rb->root = NULL;
            return;
        }
    }

    /* 此处已建立不变量：待剪除节点没有右子节点（若与后继交换过，右子指针可能未
     * 显式置空，但逻辑上成立）。此外，只有 path[0]..pathp[-1] 上的节点需要更新。
     */
    if (RB_IS_RED(pathp->node)) {
        /* 剪除红色节点，无需额外修复。 */
        assert(pathp[-1].cmp == MAP_CMP_LESS);
        rb_node_set_left(pathp[-1].node, NULL);
        return;
    }

    /* 待剪除节点为黑色，需要沿路径回溯直到恢复平衡。 */
    pathp->node = NULL;
    for (pathp--; (uintptr_t) pathp >= (uintptr_t) path; pathp--) {
        assert(pathp->cmp != MAP_CMP_EQUAL);
        if (pathp->cmp == MAP_CMP_LESS) {
            rb_node_set_left(pathp->node, pathp[1].node);
            if (RB_IS_RED(pathp->node)) {
                map_node_t *right = rb_node_get_right(pathp->node);
                map_node_t *rightleft = rb_node_get_left(right);
                map_node_t *tnode;
                if (rightleft && RB_IS_RED(rightleft)) {
                    /* 下列图中的 ||、// 和 \\ 表示通向被删除节点的路径。
                     *
                     *      ||
                     *    pathp(r)
                     *  //        \
                     * (b)        (b)
                     *           /
                     *          (r)
                     */
                    rb_node_set_black(pathp->node);
                    rb_node_rotate_right(right, tnode);
                    rb_node_set_right(pathp->node, tnode);
                    rb_node_rotate_left(pathp->node, tnode);
                } else {
                    /*      ||
                     *    pathp(r)
                     *  //        \
                     * (b)        (b)
                     *           /
                     *          (b)
                     */
                    rb_node_rotate_left(pathp->node, tnode);
                }

                /* 平衡已恢复，但旋转改变了子树根。 */
                assert((uintptr_t) pathp > (uintptr_t) path);
                if (pathp[-1].cmp == MAP_CMP_LESS)
                    rb_node_set_left(pathp[-1].node, tnode);
                else
                    rb_node_set_right(pathp[-1].node, tnode);
                return;
            } else {
                map_node_t *right = rb_node_get_right(pathp->node);
                map_node_t *rightleft = rb_node_get_left(right);
                if (rightleft && RB_IS_RED(rightleft)) {
                    /*      ||
                     *    pathp(b)
                     *  //        \
                     * (b)        (b)
                     *           /
                     *          (r)
                     */
                    map_node_t *tnode;
                    rb_node_set_black(rightleft);
                    rb_node_rotate_right(right, tnode);
                    rb_node_set_right(pathp->node, tnode);
                    rb_node_rotate_left(pathp->node, tnode);
                    /* 平衡已恢复，但旋转改变了子树根；该子树根也可能就是整棵树根。
                     */
                    if (pathp == path) {
                        /* 设置树根。 */
                        rb->root = tnode;
                    } else {
                        if (pathp[-1].cmp == MAP_CMP_LESS)
                            rb_node_set_left(pathp[-1].node, tnode);
                        else
                            rb_node_set_right(pathp[-1].node, tnode);
                    }
                    return;
                } else {
                    /*      ||
                     *    pathp(b)
                     *  //        \
                     * (b)        (b)
                     *           /
                     *          (b)
                     */
                    map_node_t *tnode;
                    rb_node_set_red(pathp->node);
                    rb_node_rotate_left(pathp->node, tnode);
                    pathp->node = tnode;
                }
            }
        } else {
            rb_node_set_right(pathp->node, pathp[1].node);
            map_node_t *left = rb_node_get_left(pathp->node);
            if (RB_IS_RED(left)) {
                map_node_t *tnode;
                map_node_t *leftright = rb_node_get_right(left);
                map_node_t *leftrightleft = rb_node_get_left(leftright);
                if (leftrightleft && RB_IS_RED(leftrightleft)) {
                    /*      ||
                     *    pathp(b)
                     *   /        \\
                     * (r)        (b)
                     *   \
                     *   (b)
                     *   /
                     * (r)
                     */
                    map_node_t *unode;
                    rb_node_set_black(leftrightleft);
                    rb_node_rotate_right(pathp->node, unode);
                    rb_node_rotate_right(pathp->node, tnode);
                    rb_node_set_right(unode, tnode);
                    rb_node_rotate_left(unode, tnode);
                } else {
                    /*      ||
                     *    pathp(b)
                     *   /        \\
                     * (r)        (b)
                     *   \
                     *   (b)
                     *   /
                     * (b)
                     */
                    assert(leftright);
                    rb_node_set_red(leftright);
                    rb_node_rotate_right(pathp->node, tnode);
                    rb_node_set_black(tnode);
                }

                /* 平衡已恢复，但旋转改变了子树根；该子树根也可能就是整棵树根。
                 */
                if (pathp == path) {
                    /* 设置树根。 */
                    rb->root = tnode;
                } else {
                    if (pathp[-1].cmp == MAP_CMP_LESS)
                        rb_node_set_left(pathp[-1].node, tnode);
                    else
                        rb_node_set_right(pathp[-1].node, tnode);
                }
                return;
            } else if (RB_IS_RED(pathp->node)) {
                map_node_t *leftleft = rb_node_get_left(left);
                if (leftleft && RB_IS_RED(leftleft)) {
                    /*        ||
                     *      pathp(r)
                     *     /        \\
                     *   (b)        (b)
                     *   /
                     * (r)
                     */
                    map_node_t *tnode;
                    rb_node_set_black(pathp->node);
                    rb_node_set_red(left);
                    rb_node_set_black(leftleft);
                    rb_node_rotate_right(pathp->node, tnode);
                    /* 平衡已恢复，但旋转改变了子树根。 */
                    assert((uintptr_t) pathp > (uintptr_t) path);
                    if (pathp[-1].cmp == MAP_CMP_LESS)
                        rb_node_set_left(pathp[-1].node, tnode);
                    else
                        rb_node_set_right(pathp[-1].node, tnode);
                    return;
                } else {
                    /*        ||
                     *      pathp(r)
                     *     /        \\
                     *   (b)        (b)
                     *   /
                     * (b)
                     */
                    rb_node_set_red(left);
                    rb_node_set_black(pathp->node);
                    /* 平衡已恢复。 */
                    return;
                }
            } else {
                map_node_t *leftleft = rb_node_get_left(left);
                if (leftleft && RB_IS_RED(leftleft)) {
                    /*               ||
                     *             pathp(b)
                     *            /        \\
                     *          (b)        (b)
                     *          /
                     *        (r)
                     */
                    map_node_t *tnode;
                    rb_node_set_black(leftleft);
                    rb_node_rotate_right(pathp->node, tnode);
                    /* 平衡已恢复，但旋转改变了子树根；该子树根也可能就是整棵树根。
                     */
                    if (pathp == path) {
                        /* 设置树根。 */
                        rb->root = tnode;
                    } else {
                        if (pathp[-1].cmp == MAP_CMP_LESS)
                            rb_node_set_left(pathp[-1].node, tnode);
                        else
                            rb_node_set_right(pathp[-1].node, tnode);
                    }
                    return;
                } else {
                    /*               ||
                     *             pathp(b)
                     *            /        \\
                     *          (b)        (b)
                     *          /
                     *        (b)
                     */
                    rb_node_set_red(left);
                }
            }
        }
    }

    /* 设置树根。 */
    rb->root = path->node;
    assert(RB_IS_BLACK(rb->root));
}

static void rb_destroy_recurse(map_t rb, map_node_t *node)
{
    if (!node)
        return;

    rb_destroy_recurse(rb, rb_node_get_left(node));
    rb_node_set_left((node), NULL);
    rb_destroy_recurse(rb, rb_node_get_right(node));
    rb_node_set_right((node), NULL);
    /* 节点、key、data 位于同一内存块，一次释放。 */
    free(node);
}

/* 用单次分配创建节点。 */
static map_node_t *map_create_node(const void *key,
                                   const void *value,
                                   size_t ksize,
                                   size_t vsize)
{
    /* 计算对齐后的 key/data 偏移。 */
    const size_t align_mask = sizeof(void *) - 1;
    size_t key_offset = (sizeof(map_node_t) + align_mask) & ~align_mask;
    size_t data_offset = (key_offset + ksize + align_mask) & ~align_mask;
    size_t total_size = data_offset + vsize;

    /* 检查大小计算是否溢出。 */
    if (unlikely(total_size < vsize || total_size < ksize))
        return NULL;

    char *mem = malloc(total_size);
    if (unlikely(!mem))
        return NULL;

    map_node_t *node = (map_node_t *) mem;
    node->key = mem + key_offset;
    node->data = mem + data_offset;

    /* 初始化节点链接。 */
    rb_node_init(node);

    /* 复制 key 和 value 数据。 */
    if (key)
        memcpy(node->key, key, ksize);
    else
        memset(node->key, 0, ksize);

    if (value)
        memcpy(node->data, value, vsize);
    else
        memset(node->data, 0, vsize);

    return node;
}

/* 构造函数：创建新的 map 实例。 */
map_t map_new(size_t key_size,
              size_t data_size,
              map_cmp_t (*cmp)(const void *, const void *))
{
    /* 校验大小，避免分配尺寸整数溢出。 */
    if (key_size == 0 || data_size == 0 || !cmp)
        return NULL;

    /* 防止溢出：确保总分配尺寸合理。 */
    size_t max_size = SIZE_MAX / 4; /* 保守上限。 */
    if (key_size > max_size || data_size > max_size ||
        (key_size + data_size) > max_size - sizeof(map_node_t))
        return NULL;

    map_t tree = malloc(sizeof(struct map_internal));
    if (!tree)
        return NULL;

    tree->key_size = key_size;
    tree->data_size = data_size;
    tree->comparator = cmp;
    tree->root = NULL;
    tree->size = 0;
    return tree;
}

/* 单次遍历插入，属于热点路径。 */
static inline const map_node_t *rb_insert_unique(map_t rb,
                                                 const void *key,
                                                 rb_path_entry_t *path,
                                                 rb_path_entry_t **pathp_out)
{
    rb_path_entry_t *pathp;

    /* 单次遍历查找插入点或已存在 key。 */
    path->node = rb->root;
    size_t depth = 0;
    for (pathp = path; pathp->node && depth < RB_MAX_DEPTH - 1;
         pathp++, depth++) {
        map_cmp_t cmp = pathp->cmp = (rb->comparator)(key, pathp->node->key);
        if (cmp == MAP_CMP_LESS) {
            pathp[1].node = rb_node_get_left(pathp->node);
        } else if (cmp == MAP_CMP_GREATER) {
            pathp[1].node = rb_node_get_right(pathp->node);
        } else {
            /* key 已存在。 */
            return pathp->node;
        }
    }

    /* key 不存在，返回 NULL 并设置 pathp 供插入使用。 */
    if (depth >= RB_MAX_DEPTH - 1)
        return (const map_node_t *) -1; /* 树太深。 */
    *pathp_out = pathp;
    return NULL;
}

/* 向 map 插入 key-value 对。 */
bool map_insert(map_t obj, const void *key, const void *val)
{
    if (!obj || !key)
        return false;

    rb_path_entry_t path[RB_MAX_DEPTH];
    rb_path_entry_t *pathp = NULL;

    /* 单次遍历检查是否存在并取得插入点。 */
    const map_node_t *existing = rb_insert_unique(obj, key, path, &pathp);
    if (existing == (const map_node_t *) -1)
        return false; /* 树太深。 */
    if (existing)
        return false; /* key 已存在。 */

    /* 创建并插入新节点。 */
    map_node_t *node = map_create_node(key, val, obj->key_size, obj->data_size);
    if (!node)
        return false;

    /* 节点已在 map_create_node 中初始化，这里只需写入路径。 */
    pathp->node = node;

    /* 修复红黑树性质。 */
    for (pathp--; (uintptr_t) pathp >= (uintptr_t) path; pathp--) {
        map_node_t *cnode = pathp->node;
        if (pathp->cmp == MAP_CMP_LESS) {
            map_node_t *left = pathp[1].node;
            rb_node_set_left(cnode, left);
            if (RB_IS_BLACK(left))
                break;
            map_node_t *leftleft = rb_node_get_left(left);
            if (leftleft && RB_IS_RED(leftleft)) {
                /* 修复 4-node。 */
                map_node_t *tnode;
                rb_node_set_black(leftleft);
                rb_node_rotate_right(cnode, tnode);
                cnode = tnode;
            }
        } else {
            map_node_t *right = pathp[1].node;
            rb_node_set_right(cnode, right);
            if (RB_IS_BLACK(right))
                break;
            map_node_t *left = rb_node_get_left(cnode);
            if (left && RB_IS_RED(left)) {
                /* 拆分 4-node。 */
                rb_node_set_black(left);
                rb_node_set_black(right);
                rb_node_set_red(cnode);
            } else {
                /* 调整为左倾。 */
                map_node_t *tnode;
                map_color_t tcolor = rb_node_get_color(cnode);
                rb_node_rotate_left(cnode, tnode);
                rb_node_set_color(tnode, tcolor);
                rb_node_set_red(cnode);
                cnode = tnode;
            }
        }
        pathp->node = cnode;
    }

    /* 设置根节点并染黑。 */
    obj->root = path->node;
    rb_node_set_black(obj->root);
    obj->size++;
    return true;
}

/* 查找函数，避免栈上路径分配。 */
void map_find(map_t obj, map_iter_t *it, const void *key)
{
    if (unlikely(!obj || !it)) {
        if (it)
            it->node = NULL;
        return;
    }

    map_node_t *node = obj->root;

    /* 大树场景提前预取。 */
    if (node && obj->size > 10000) {
        PREFETCH_READ(node->left);
        PREFETCH_READ(rb_node_get_right(node));
    }

    while (node) {
        map_cmp_t cmp = obj->comparator(key, node->key);
        if (cmp == MAP_CMP_EQUAL) {
            it->node = node;
            return;
        }
        node = (cmp == MAP_CMP_LESS) ? node->left : rb_node_get_right(node);
    }
    it->node = NULL;
}

bool map_empty(map_t obj)
{
    return unlikely(!obj) || !obj->root;
}

/* 迭代。 */
bool map_at_end(map_t m, const map_iter_t *it)
{
    (void) m; /* 抑制未使用参数警告。 */
    return !(it->node);
}

/* 删除函数。 */
void map_erase(map_t obj, map_iter_t *it)
{
    if (!obj || !it || !it->node)
        return;

    /* 删除前确认树中仍有节点。 */
    if (obj->size == 0)
        return;

    rb_remove(obj, it->node);
    /* 节点、key、data 位于同一内存块，一次释放。 */
    free(it->node);
    it->node = NULL;

    /* 防止 size 下溢。 */
    if (obj->size > 0)
        obj->size--;
}

/* 清空 map。 */
void map_clear(map_t obj)
{
    if (!obj)
        return;
    rb_destroy_recurse(obj, obj->root);
    obj->root = NULL;
    obj->size = 0;
}

/* 销毁 map 并释放所有资源。 */
void map_delete(map_t obj)
{
    if (!obj)
        return;
    map_clear(obj);
    free(obj);
}

/* 获取 map 中元素数量。 */
size_t map_size(map_t obj)
{
    return likely(obj) ? obj->size : 0;
}

/* 迭代器遍历函数。 */

void map_first(map_t map, map_iter_t *it)
{
    if (unlikely(!map || !it)) {
        if (it)
            it->node = NULL;
        return;
    }

    map_node_t *node = map->root;
    if (likely(node)) {
        while (node->left)
            node = node->left;
    }

    it->node = node;
    it->prev = NULL;
    it->count = 0;
}

void map_last(map_t map, map_iter_t *it)
{
    if (unlikely(!map || !it)) {
        if (it)
            it->node = NULL;
        return;
    }

    map_node_t *node = map->root;
    if (likely(node)) {
        map_node_t *right;
        while ((right = rb_node_get_right(node)))
            node = right;
    }

    it->node = node;
    it->prev = NULL;
    it->count = 0;
}

void map_next(map_t map, map_iter_t *it)
{
    if (unlikely(!map || !it || !it->node)) {
        if (it)
            it->node = NULL;
        return;
    }

    map_node_t *node = it->node;
    map_node_t *right = rb_node_get_right(node);

    /* 如果存在右子树，后继为右子树最左节点。 */
    if (right) {
        while (right->left)
            right = right->left;
        it->node = right;
        return;
    }

    /* 从根节点搜索后继。 */
    map_node_t *succ = NULL;
    map_node_t *curr = map->root;

    while (curr) {
        map_cmp_t cmp = map->comparator(it->node->key, curr->key);
        if (cmp == MAP_CMP_LESS) {
            succ = curr;
            curr = curr->left;
        } else if (cmp == MAP_CMP_GREATER) {
            curr = rb_node_get_right(curr);
        } else {
            break;
        }
    }

    it->node = succ;
}

void map_prev(map_t map, map_iter_t *it)
{
    if (!map || !it || !it->node) {
        if (it)
            it->node = NULL;
        return;
    }

    map_node_t *node = it->node;

    /* 如果存在左子树，前驱为左子树最右节点。 */
    if (node->left) {
        node = node->left;
        while (rb_node_get_right(node))
            node = rb_node_get_right(node);
        it->node = node;
        return;
    }

    /* 否则需要找到第一个“当前节点位于其右子树”的祖先。 */
    /* 没有父指针，因此从根节点重新搜索前驱。 */

    map_node_t *pred = NULL;
    map_node_t *curr = map->root;

    while (curr) {
        map_cmp_t cmp = map->comparator(it->node->key, curr->key);
        if (cmp == MAP_CMP_GREATER) {
            pred = curr;
            curr = rb_node_get_right(curr);
        } else if (cmp == MAP_CMP_LESS) {
            curr = curr->left;
        } else {
            /* 已找到节点，前驱已设置或为 NULL。 */
            break;
        }
    }

    it->node = pred;
}
