/*
 * rv32emu is freely redistributable under the MIT License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 */

/*
 * virtio-blk MMIO 块设备模型。
 *
 * Linux 客体通过 virtqueue 提交块读写请求，本文件解析描述符链，把请求映射到
 * 宿主磁盘镜像文件，并在完成后更新 used ring 与中断状态。实现重点覆盖单队列
 * virtio-mmio 块设备，供系统模式挂载 rootfs 或额外磁盘使用。
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * /dev/ 下的块设备无法打包进 wasm 产物，因此 wasm 构建不支持直接访问
 * /dev/ 块设备。
 */
#if !defined(__EMSCRIPTEN__)
#if defined(__APPLE__)
#include <sys/disk.h> /* DKIOCGETBLOCKCOUNT 和 DKIOCGETBLOCKSIZE */
#else
#include <linux/fs.h> /* BLKGETSIZE64 */
#endif
#endif /* !defined(__EMSCRIPTEN__) */

#include "virtio.h"

#define DISK_BLK_SIZE 512

#define VBLK_FEATURES_0 0
#define VBLK_FEATURES_1 1 /* VIRTIO_F_VERSION_1 */
#define VBLK_QUEUE_NUM_MAX 1024
#define VBLK_QUEUE (vblk->queues[vblk->queue_sel])

#define VBLK_PRIV(x) ((struct virtio_blk_config *) x->priv)

PACKED(struct virtio_blk_config {
    uint64_t capacity;
    uint32_t size_max;
    uint32_t seg_max;

    struct virtio_blk_geometry {
        uint16_t cylinders;
        uint8_t heads;
        uint8_t sectors;
    } geometry;

    uint32_t blk_size;

    struct virtio_blk_topology {
        uint8_t physical_block_exp;
        uint8_t alignment_offset;
        uint16_t min_io_size;
        uint32_t opt_io_size;
    } topology;

    uint8_t writeback;
    uint8_t unused0[3];
    uint32_t max_discard_sectors;
    uint32_t max_discard_seg;
    uint32_t discard_sector_alignment;
    uint32_t max_write_zeroes_sectors;
    uint32_t max_write_zeroes_seg;
    uint8_t write_zeroes_may_unmap;
    uint8_t unused1[3];
    uint64_t disk_size;
});

PACKED(struct vblk_req_header {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
    uint8_t status;
});

static void virtio_blk_set_fail(virtio_blk_state_t *vblk)
{
    vblk->status |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;
    if (vblk->status & VIRTIO_STATUS_DRIVER_OK)
        vblk->interrupt_status |= VIRTIO_INT_CONF_CHANGE;
}

static inline uint32_t vblk_preprocess(virtio_blk_state_t *vblk UNUSED,
                                       uint32_t addr)
{
    /* 当 MEM_SIZE 为 4GB 时，所有 32 位地址天然都在边界内。
     * 这里用编译期条件避免 GCC 的 -Wtype-limits 警告。
     */
#if MEM_SIZE < 0x100000000ULL
    if ((addr >= MEM_SIZE) || (addr & 0b11)) {
#else
    if (addr & 0b11) {
#endif
        virtio_blk_set_fail(vblk);
        return 0;
    }

    return addr >> 2;
}

static void virtio_blk_update_status(virtio_blk_state_t *vblk, uint32_t status)
{
    vblk->status |= status;
    if (status)
        return;

    /* 状态寄存器写 0 表示设备复位；保留外部资源，清空运行时协商状态。 */
    uint32_t device_features = vblk->device_features;
    uint32_t *ram = vblk->ram;
    uint32_t *disk = vblk->disk;
    uint64_t disk_size = vblk->disk_size;
    int disk_fd = vblk->disk_fd;
    void *priv = vblk->priv;
    uint32_t capacity = VBLK_PRIV(vblk)->capacity;
    memset(vblk, 0, sizeof(*vblk));
    vblk->device_features = device_features;
    vblk->ram = ram;
    vblk->disk = disk;
    vblk->disk_size = disk_size;
    vblk->disk_fd = disk_fd;
    vblk->priv = priv;
    VBLK_PRIV(vblk)->capacity = capacity;
}

static void virtio_blk_write_handler(virtio_blk_state_t *vblk,
                                     uint64_t sector,
                                     uint64_t desc_addr,
                                     uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    const void *src = (void *) ((uintptr_t) vblk->ram + desc_addr);
    memcpy(dest, src, len);
}

static void virtio_blk_read_handler(virtio_blk_state_t *vblk,
                                    uint64_t sector,
                                    uint64_t desc_addr,
                                    uint32_t len)
{
    void *dest = (void *) ((uintptr_t) vblk->ram + desc_addr);
    const void *src =
        (void *) ((uintptr_t) vblk->disk + sector * DISK_BLK_SIZE);
    memcpy(dest, src, len);
}

static int virtio_blk_desc_handler(virtio_blk_state_t *vblk,
                                   const virtio_blk_queue_t *queue,
                                   uint16_t desc_idx,
                                   uint32_t *plen)
{
    /* 一个完整的 virtio_blk_req 由 3 个描述符表示：
     * 第一个描述符包含：
     *   le32 type
     *   le32 reserved
     *   le64 sector
     * 第二个描述符包含：
     *   u8 data[][512]
     * 第三个描述符包含：
     *   u8 status
     */
    struct virtq_desc vq_desc[3];

    /* 沿 next 链收集三个描述符。 */
    for (int i = 0; i < 3; i++) {
        /* struct virtq_desc 大小为 4 个 32 位字。 */
        const struct virtq_desc *desc =
            (struct virtq_desc *) &vblk->ram[queue->queue_desc + desc_idx * 4];

        /* 取出当前描述符字段，避免后续处理受 guest 内存变化影响。 */
        vq_desc[i].addr = desc->addr;
        vq_desc[i].len = desc->len;
        vq_desc[i].flags = desc->flags;
        desc_idx = desc->next;
    }

    /* 前两个描述符必须设置 NEXT，第三个描述符必须是链表末尾。
     */
    if (!(vq_desc[0].flags & VIRTIO_DESC_F_NEXT) ||
        !(vq_desc[1].flags & VIRTIO_DESC_F_NEXT) ||
        (vq_desc[2].flags & VIRTIO_DESC_F_NEXT)) {
        /* 描述符链异常时不回写 status，直接让设备进入需要复位状态。 */
        virtio_blk_set_fail(vblk);
        return -1;
    }

    /* 解析请求头，得到操作类型和扇区号。 */
    const struct vblk_req_header *header =
        (struct vblk_req_header *) ((uintptr_t) vblk->ram + vq_desc[0].addr);
    uint32_t type = header->type;
    uint64_t sector = header->sector;
    uint8_t *status = (uint8_t *) ((uintptr_t) vblk->ram + vq_desc[2].addr);

    /* 检查扇区索引是否落在设备容量内。 */
    if (sector > (VBLK_PRIV(vblk)->capacity - 1)) {
        *status = VIRTIO_BLK_S_IOERR;
        return -1;
    }

    /* 按请求类型读写数据区。 */
    switch (type) {
    case VIRTIO_BLK_T_IN:
        virtio_blk_read_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    case VIRTIO_BLK_T_OUT:
        if (vblk->device_features & VIRTIO_BLK_F_RO) { /* 只读设备 */
            rv_log_error("尝试写入只读块设备");
            *status = VIRTIO_BLK_S_IOERR;
            return -1;
        }
        virtio_blk_write_handler(vblk, sector, vq_desc[1].addr, vq_desc[1].len);
        break;
    default:
        rv_log_error("不支持的 virtio-blk 操作");
        *status = VIRTIO_BLK_S_UNSUPP;
        return -1;
    }

    /* 向第三个描述符回写设备处理结果。 */
    *status = VIRTIO_BLK_S_OK;
    *plen = vq_desc[1].len;

    return 0;
}

static void virtio_queue_notify_handler(virtio_blk_state_t *vblk, int index)
{
    uint32_t *ram = vblk->ram;
    virtio_blk_queue_t *queue = &vblk->queues[index];
    if (vblk->status & VIRTIO_STATUS_DEVICE_NEEDS_RESET)
        return;

    if (!((vblk->status & VIRTIO_STATUS_DRIVER_OK) && queue->ready))
        return virtio_blk_set_fail(vblk);

    /* 检查 available queue 中是否有新缓冲区。 */
    uint16_t new_avail = ram[queue->queue_avail] >> 16;
    if (new_avail - queue->last_avail > (uint16_t) queue->queue_num) {
        rv_log_error("大小检查失败");
        return virtio_blk_set_fail(vblk);
    }

    if (queue->last_avail == new_avail)
        return;

    /* 逐个处理 guest 新提交的请求。 */
    uint16_t new_used =
        ram[queue->queue_used] >> 16; /* virtq_used.idx (le16) */
    while (queue->last_avail != new_avail) {
        /* 计算 ring buffer 中的索引。 */
        uint16_t queue_idx = queue->last_avail % queue->queue_num;

        /* 每个 buffer index 占 2 字节，而这里以 4 字节为单位访问内存。
         * available queue 的第一个元素位于 ram[queue->queue_avail + 1]，
         * 因此需要通过数组下标和位移取出对应的 16 位 index。可对照规范中的
         * struct virtq_avail。
         */
        uint16_t buffer_idx = ram[queue->queue_avail + 1 + queue_idx / 2] >>
                              (16 * (queue_idx % 2));

        /* 消费 available queue 中的请求，并处理描述符链里的数据。 */
        uint32_t len = 0;
        int result = virtio_blk_desc_handler(vblk, queue, buffer_idx, &len);
        if (result != 0)
            return virtio_blk_set_fail(vblk);

        /* 把 used element 信息（struct virtq_used_elem）写入 used queue。 */
        uint32_t vq_used_addr =
            queue->queue_used + 1 + (new_used % queue->queue_num) * 2;
        ram[vq_used_addr] = buffer_idx; /* virtq_used_elem.id  (le32) */
        ram[vq_used_addr + 1] = len;    /* virtq_used_elem.len (le32) */
        queue->last_avail++;
        new_used++;
    }

    /* 参照规范中的 struct virtq_used_elem.le32 len 字段。 */
    vblk->ram[queue->queue_used] &= MASK(16); /* 清零高 16 位 idx 区域。 */
    vblk->ram[queue->queue_used] |= ((uint32_t) new_used) << 16; /* idx */

    /* 除非设置了 VIRTQ_AVAIL_F_NO_INTERRUPT，否则发送 used-ring 中断。 */
    if (!(ram[queue->queue_avail] & 1))
        vblk->interrupt_status |= VIRTIO_INT_USED_RING;
}

uint32_t virtio_blk_read(virtio_blk_state_t *vblk, uint32_t addr)
{
    addr = addr >> 2;
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(MagicValue):
        return VIRTIO_MAGIC_NUMBER;
    case _(Version):
        return VIRTIO_VERSION;
    case _(DeviceID):
        return VIRTIO_BLK_DEV_ID;
    case _(VendorID):
        return VIRTIO_VENDOR_ID;
    case _(DeviceFeatures):
        return vblk->device_features_sel == 0
                   ? VBLK_FEATURES_0 | vblk->device_features
                   : (vblk->device_features_sel == 1 ? VBLK_FEATURES_1 : 0);
    case _(QueueNumMax):
        return VBLK_QUEUE_NUM_MAX;
    case _(QueueReady):
        return (uint32_t) VBLK_QUEUE.ready;
    case _(InterruptStatus):
        return vblk->interrupt_status;
    case _(Status):
        return vblk->status;
    case _(ConfigGeneration):
        return VIRTIO_CONFIG_GENERATE;
    default:
        /* 从对应配置寄存器读取。 */
        return ((uint32_t *) VBLK_PRIV(vblk))[addr - _(Config)];
    }
#undef _
}

void virtio_blk_write(virtio_blk_state_t *vblk, uint32_t addr, uint32_t value)
{
    addr = addr >> 2;
#define _(reg) VIRTIO_##reg
    switch (addr) {
    case _(DeviceFeaturesSel):
        vblk->device_features_sel = value;
        break;
    case _(DriverFeatures):
        vblk->driver_features_sel == 0 ? (vblk->driver_features = value) : 0;
        break;
    case _(DriverFeaturesSel):
        vblk->driver_features_sel = value;
        break;
    case _(QueueSel):
        if (value < ARRAY_SIZE(vblk->queues))
            vblk->queue_sel = value;
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueNum):
        if (value > 0 && value <= VBLK_QUEUE_NUM_MAX)
            VBLK_QUEUE.queue_num = value;
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueReady):
        VBLK_QUEUE.ready = value & 1;
        if (value & 1)
            VBLK_QUEUE.last_avail = vblk->ram[VBLK_QUEUE.queue_avail] >> 16;
        break;
    case _(QueueDescLow):
        VBLK_QUEUE.queue_desc = vblk_preprocess(vblk, value);
        break;
    case _(QueueDescHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueDriverLow):
        VBLK_QUEUE.queue_avail = vblk_preprocess(vblk, value);
        break;
    case _(QueueDriverHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueDeviceLow):
        VBLK_QUEUE.queue_used = vblk_preprocess(vblk, value);
        break;
    case _(QueueDeviceHigh):
        if (value)
            virtio_blk_set_fail(vblk);
        break;
    case _(QueueNotify):
        if (value < ARRAY_SIZE(vblk->queues))
            virtio_queue_notify_handler(vblk, value);
        else
            virtio_blk_set_fail(vblk);
        break;
    case _(InterruptACK):
        vblk->interrupt_status &= ~value;
        break;
    case _(Status):
        virtio_blk_update_status(vblk, value);
        break;
    default:
        /* 写入对应配置寄存器。 */
        ((uint32_t *) VBLK_PRIV(vblk))[addr - _(Config)] = value;
        break;
    }
#undef _
}

uint32_t *virtio_blk_init(virtio_blk_state_t *vblk,
                          char *disk_file,
                          bool readonly)
{
    /*
     * mmap_fallback 模式下，如果没有实际指定 vblk，disk_fd 应保持为 -1，
     * 退出时也不需要 fsync。
     */

    vblk->disk_fd = -1;

    /* 分配 virtio-blk 私有配置区。 */
    vblk->priv = calloc(1, sizeof(struct virtio_blk_config));
    assert(vblk->priv);

    /* 未提供磁盘镜像。 */
    if (!disk_file) {
        /* 容量置零后，内核启动后不会继续访问该块设备。 */
        VBLK_PRIV(vblk)->capacity = 0;
        return NULL;
    }

    /* 打开磁盘镜像或块设备文件。 */
    int disk_fd = open(disk_file, readonly ? O_RDONLY : O_RDWR);
    if (disk_fd < 0) {
        rv_log_error("无法打开 %s：%s", disk_file, strerror(errno));
        goto fail;
    }

    struct stat st;
    if (fstat(disk_fd, &st) == -1) {
        rv_log_error("fstat 失败：%s", strerror(errno));
        goto disk_size_fail;
    }

    const char *disk_file_dirname = dirname(disk_file);
    if (!disk_file_dirname) {
        rv_log_error("dirname 处理磁盘文件失败：%s：%s", disk_file,
                     strerror(errno));
        goto disk_size_fail;
    }
    /* 获取磁盘大小。 */
    uint64_t disk_size;
    if (!strcmp(disk_file_dirname, "/dev")) { /* /dev/ 路径使用 ioctl 查询。 */
        if ((st.st_mode & S_IFMT) != S_IFBLK) {
            rv_log_error("%s 不是块设备", disk_file);
            goto fail;
        }
#if !defined(__EMSCRIPTEN__)
#if defined(__APPLE__)
        uint32_t block_size;
        uint64_t block_count;
        if (ioctl(disk_fd, DKIOCGETBLOCKCOUNT, &block_count) == -1) {
            rv_log_error("DKIOCGETBLOCKCOUNT 失败：%s", strerror(errno));
            goto disk_size_fail;
        }
        if (ioctl(disk_fd, DKIOCGETBLOCKSIZE, &block_size) == -1) {
            rv_log_error("DKIOCGETBLOCKSIZE 失败：%s", strerror(errno));
            goto disk_size_fail;
        }
        disk_size = block_count * block_size;
#else /* Linux */
        if (ioctl(disk_fd, BLKGETSIZE64, &disk_size) == -1) {
            rv_log_error("BLKGETSIZE64 失败：%s", strerror(errno));
            goto disk_size_fail;
        }
#endif
#endif       /* !defined(__EMSCRIPTEN__) */
    } else { /* 普通路径通过 stat 结果读取文件大小。 */
        disk_size = st.st_size;
    }
    VBLK_PRIV(vblk)->disk_size = disk_size;

    /* 建立磁盘内容映射。 */
    uint32_t *disk_mem;
#if HAVE_MMAP
    disk_mem = mmap(NULL, VBLK_PRIV(vblk)->disk_size,
                    readonly ? PROT_READ : (PROT_READ | PROT_WRITE), MAP_SHARED,
                    disk_fd, 0);
    if (disk_mem == MAP_FAILED) {
        if (errno != EINVAL)
            goto disk_mem_err;
        /*
         * Apple 平台上块设备 mmap() 似乎不受支持，errno 会被设置为 EINVAL。
         */
        rv_log_trace(
            "mmap() 失败，回退到基于 malloc 的块设备缓冲区");
        goto mmap_fallback;
    }
    /*
     * 使用 mmap_fallback 时，退出前需要把堆内存刷回设备后再关闭 disk_fd。
     */
    close(disk_fd);
    goto disk_mem_ok;
#endif

mmap_fallback:
    disk_mem = malloc(VBLK_PRIV(vblk)->disk_size);
    if (!disk_mem)
        goto disk_mem_err;
    vblk->disk_fd = disk_fd;
    vblk->disk_size = disk_size;
    if (pread(disk_fd, disk_mem, disk_size, 0) == -1) {
        rv_log_error("读取块设备失败：%s", strerror(errno));
        goto disk_mem_err;
    }

disk_mem_ok:
    assert(!(((uintptr_t) disk_mem) & 0b11));

    vblk->disk = disk_mem;
    VBLK_PRIV(vblk)->capacity =
        (VBLK_PRIV(vblk)->disk_size - 1) / DISK_BLK_SIZE + 1;

    if (readonly)
        vblk->device_features = VIRTIO_BLK_F_RO;

    return disk_mem;

disk_mem_err:
    rv_log_error("无法映射磁盘 %s：%s", disk_file, strerror(errno));

disk_size_fail:
    close(disk_fd);

fail:
    exit(EXIT_FAILURE);
}

virtio_blk_state_t *vblk_new()
{
    virtio_blk_state_t *vblk = calloc(1, sizeof(virtio_blk_state_t));
    assert(vblk);
    return vblk;
}

void vblk_delete(virtio_blk_state_t *vblk)
{
    /* mmap_fallback 模式使用 malloc 分配的缓冲区。 */
    if (vblk->disk_fd != -1)
        free(vblk->disk);
#if HAVE_MMAP
    else
        munmap(vblk->disk, VBLK_PRIV(vblk)->disk_size);
#endif
    free(vblk->priv);
    free(vblk);
}
