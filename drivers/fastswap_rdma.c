#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/inet.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/fastswap.h>
#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include "fastswap.h"

#define CONNECT_TIMEOUT_MS (60000)
#define WAIT_TIMEOUT_MS (10000)
#define QUEUE_DEPTH (1024)

enum queue_type { QP_READ_SYNC, QP_READ_ASYNC, QP_WRITE, QP_TYPES };

/* Represents one page (swap-in/swap-out) requrest */
struct fastswap_request {
    struct ib_cqe completion_callback;
    struct folio *folio;
    dma_addr_t local_mapped_addr;
};

/* Metadata for one registered connection */
struct fastswap_queue {
    struct rdma_cm_id *connection;
    struct ib_cq *completion_queue;
    enum ib_poll_context poll_mode;
    spinlock_t completion_queue_lock;
    atomic_t requests_in_flight;
    bool is_failed;
    struct fastswap_request *requests;
    DECLARE_BITMAP(used_request_map, QUEUE_DEPTH);
    struct completion connect_completion;
    int connect_error;
};

/* RDMA memory region metadata returned by Remote Server
 * little-endian */
struct remote_region {
    u64 base_addr;
    u64 size;
    u32 remote_key;
    u32 padding;
};

static char server_ip[INET_ADDRSTRLEN];
static char client_ip[INET_ADDRSTRLEN];
static char server_port[6];
static int queues;

/* Module kparams are visible but not writable.
 * Can be read from /sys/module/fastswp/ */
module_param_string(server_ip, server_ip, sizeof(server_ip), 0444);
module_param_string(client_ip, client_ip, sizeof(client_ip), 0444);
module_param_string(server_port, server_port, sizeof(server_port), 0444);
module_param(queues, int, 0444);

/* One PD per device to device relationship */
static struct ib_pd *protection_domain;
static struct fastswap_queue *all_queues;
static struct remote_region server_region;
static bool is_closing;
static struct sockaddr_storage server_addr, client_addr;

/* CPU may change before the request is posted; the queue is only a hint */
static struct fastswap_queue *queue_for_cpu(int cpu, enum queue_type kind)
{
    return &all_queues[kind * queues + cpu % queues];
}

static int create_queue_resources(struct fastswap_queue *queue)
{
    struct ib_device *device = queue->connection->device;
    struct ib_qp_init_attr qp_attributes = {
        .cap.max_send_wr = QUEUE_DEPTH, /* send request buffer size */
        .cap.max_recv_wr = 1,
        .cap.max_send_sge = 1, /* one request may reference upto "1"
                                  (local IO-VA address, length, lkey) tuple */
        .cap.max_recv_sge = 1,
        .sq_sig_type = IB_SIGNAL_REQ_WR, /* only tagged send requests produce completion events */
        .qp_type = IB_QPT_RC, /* reliable conenction */
    };
    int ret;

    /* We treat the remote memory as one big pool. Hence we only allocate one PD */
    if (!protection_domain) {
        protection_domain = ib_alloc_pd(device, 0);
        if (IS_ERR(protection_domain)) {
            ret = PTR_ERR(protection_domain);
            protection_domain = NULL;
            return ret;
        }
    }

    /* GFP - gets free pages */
    queue->requests = kcalloc(QUEUE_DEPTH, sizeof(*queue->requests), GFP_KERNEL);
    if (!queue->requests) {
        return -ENOMEM;
    }

    /* Completion queue holds posts: send (QUEUE_DEPTH) + recieve (1) */
    queue->completion_queue = ib_alloc_cq(device, queue, QUEUE_DEPTH + 1, 0, queue->poll_mode);
    if (IS_ERR(queue->completion_queue)) {
        ret = PTR_ERR(queue->completion_queue);
        queue->completion_queue = NULL;
        return ret;
    }

    qp_attributes.send_cq = queue->completion_queue;
    qp_attributes.recv_cq = queue->completion_queue;

    return rdma_create_qp(queue->connection, protection_domain, &qp_attributes);
}

static int connection_event_handler(struct rdma_cm_id *cm_id, struct rdma_cm_event *event)
{
    struct fastswap_queue *queue = cm_id->context;
    static struct rdma_conn_param connect_params = {
        .flow_control = 1,
        .responder_resources = 16,
        .initiator_depth = 16,
        .retry_count = 6,
        .rnr_retry_count = 7,
    };
    int err = 0;

    switch (event->event) {
        case RDMA_CM_EVENT_ADDR_RESOLVED:
            err = create_queue_resources(queue) ?: rdma_resolve_route(cm_id, CONNECT_TIMEOUT_MS);
            break;
        case RDMA_CM_EVENT_ROUTE_RESOLVED:
            /* CM Handler has already acquired handler_mutex, so must call (already) locked variant
             * rdma_conenct would attempt to acquire lock */
            err = rdma_connect_locked(cm_id, &connect_params);
            break;
        case RDMA_CM_EVENT_ESTABLISHED:
            if (event->param.conn.private_data_len < sizeof(server_region)) {
                err = -EPROTO;
            } else {
                memcpy(&server_region, event->param.conn.private_data, sizeof(server_region));
            }
            break;
        default:
            if (READ_ONCE(is_closing)) {
                return 0;
            }

            pr_err("cm event %s (%d)\n", rdma_event_msg(event->event), event->status);
            queue->is_failed = true;
            err = -ECONNRESET;
    }

    if (err || event->event == RDMA_CM_EVENT_ESTABLISHED) {
        queue->connect_error = err;
        complete(&queue->connect_completion);
    }

    return 0;
}

static int connect_queue(struct fastswap_queue *queue, enum ib_poll_context poll_mode)
{
    queue->poll_mode = poll_mode;
    queue->connect_error = -ETIMEDOUT;

    spin_lock_init(&queue->completion_queue_lock);
    init_completion(&queue->connect_completion);

    queue->connection = rdma_create_id(&init_net, connection_event_handler, queue,
            RDMA_PS_TCP /* TCP like Port Space */, IB_QPT_RC);
    if (IS_ERR(queue->connection)) {
        return PTR_ERR(queue->connection);
    }

    if (rdma_resolve_addr(queue->connection, client_ip[0] ? (struct sockaddr *)&client_addr : NULL,
                (struct sockaddr *)&server_addr, CONNECT_TIMEOUT_MS)) {
        return -ENODEV;
    }

    /* connection_event_handler is called on establishing connection. Wait for completion.  */
    wait_for_completion_timeout(&queue->connect_completion, msecs_to_jiffies(CONNECT_TIMEOUT_MS) + 1);

    return queue->connect_error;
}

static void close_queues(void)
{
    int i;

    WRITE_ONCE(is_closing, true);

    for (i = 0; all_queues && i < QP_TYPES * queues; i++) {
        struct fastswap_queue *queue = &all_queues[i];

        if (IS_ERR_OR_NULL(queue->connection)) {
            continue;
        }

        rdma_disconnect(queue->connection);
        if (queue->connection->qp) {
            rdma_destroy_qp(queue->connection);
        }

        if (queue->completion_queue) {
            ib_free_cq(queue->completion_queue);
        }

        rdma_destroy_id(queue->connection);
        kfree(queue->requests);
    }

    if (protection_domain) {
        ib_dealloc_pd(protection_domain);
    }

    kfree(all_queues);
}

/* one step of a bounded wait; true once the deadline has passed */
static bool wait_step(struct fastswap_queue *queue, unsigned long deadline)
{
    unsigned long irq_flags;

    if (queue->poll_mode == IB_POLL_DIRECT) {
        spin_lock_irqsave(&queue->completion_queue_lock, irq_flags);
        ib_process_cq_direct(queue->completion_queue, 16);
        spin_unlock_irqrestore(&queue->completion_queue_lock, irq_flags);
    }

    cond_resched();

    return time_after(jiffies, deadline);
}

static int wait_in_flight(struct fastswap_queue *queue)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(WAIT_TIMEOUT_MS);

    while (atomic_read(&queue->requests_in_flight)) {
        if (wait_step(queue, deadline)) {
            pr_err("timed out, %d requests pending\n", atomic_read(&queue->requests_in_flight));
            return -ETIMEDOUT;
        }
    }

    return 0;
}

static void complete_folio(struct folio *folio, bool is_uptodate)
{
    if (is_uptodate) {
        folio_mark_uptodate(folio);
    }

    folio_unlock(folio);
}

static void release_request(struct fastswap_queue *queue, struct fastswap_request *request,
        enum dma_data_direction direction)
{
    ib_dma_unmap_page(queue->connection->device, request->local_mapped_addr,
            folio_size(request->folio), direction);
    clear_bit(request - queue->requests, queue->used_request_map);
    atomic_dec(&queue->requests_in_flight);
}

static void write_done(struct ib_cq *completion_queue, struct ib_wc *work_completion)
{
    struct fastswap_queue *queue = completion_queue->cq_context;

    if (work_completion->status != IB_WC_SUCCESS) {
        pr_err("write failed: %d\n", work_completion->status);
        queue->is_failed = true;
    }

    release_request(queue, container_of(work_completion->wr_cqe, struct fastswap_request, completion_callback),
            DMA_TO_DEVICE);
}

static void read_done(struct ib_cq *completion_queue, struct ib_wc *work_completion)
{
    struct fastswap_queue *queue = completion_queue->cq_context;
    struct fastswap_request *request =
        container_of(work_completion->wr_cqe, struct fastswap_request, completion_callback);
    struct folio *folio = request->folio;

    if (work_completion->status != IB_WC_SUCCESS) {
        pr_err("read failed: %d\n", work_completion->status);
        queue->is_failed = true;
    }

    release_request(queue, request, DMA_FROM_DEVICE);
    complete_folio(folio, work_completion->status == IB_WC_SUCCESS);
}

static int post_page(struct fastswap_queue *queue, struct folio *folio, u64 remote_offset,
        enum ib_wr_opcode opcode)
{
    struct ib_device *device = queue->connection->device;
    enum dma_data_direction direction = opcode == IB_WR_RDMA_WRITE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
    unsigned long deadline = jiffies + msecs_to_jiffies(WAIT_TIMEOUT_MS);
    struct fastswap_request *request;
    const struct ib_send_wr *failed_work_request;
    struct ib_sge local_segment;
    struct ib_rdma_wr work_request = {};
    size_t size = folio_size(folio);
    int request_index, ret;

    if (queue->is_failed) {
        return -EIO;
    }

    if (remote_offset + size > server_region.size) {
        return -ENOSPC;
    }

    while ((request_index = find_first_zero_bit(queue->used_request_map, QUEUE_DEPTH)) >= QUEUE_DEPTH ||
            test_and_set_bit(request_index, queue->used_request_map)) { 
        if (request_index >= QUEUE_DEPTH && wait_step(queue, deadline)) {
            return -ETIMEDOUT;
        }
    }

    request = &queue->requests[request_index];
    request->folio = folio;
    request->completion_callback.done = opcode == IB_WR_RDMA_WRITE ? write_done : read_done;
    request->local_mapped_addr = ib_dma_map_page(device, folio_page(folio, 0), 0, size,
            direction);

    if (ib_dma_mapping_error(device, request->local_mapped_addr)) {
        clear_bit(request_index, queue->used_request_map);
        return -ENOMEM;
    }

    local_segment.addr = request->local_mapped_addr;
    local_segment.length = size;
    local_segment.lkey = protection_domain->local_dma_lkey;
    work_request.wr.wr_cqe = &request->completion_callback;
    work_request.wr.sg_list = &local_segment;
    work_request.wr.num_sge = 1;
    work_request.wr.opcode = opcode;
    work_request.wr.send_flags = IB_SEND_SIGNALED;
    work_request.remote_addr = server_region.base_addr + remote_offset;
    work_request.rkey = server_region.remote_key;

    atomic_inc(&queue->requests_in_flight);
    ret = ib_post_send(queue->connection->qp, &work_request.wr, &failed_work_request);

    if (ret) {
        pr_err("ib_post_send failed: %d\n", ret);
        release_request(queue, request, direction);
    }

    return ret;
}

static int rdma_store(unsigned swap_type, pgoff_t swap_offset, struct folio *folio)
{
    struct fastswap_queue *queue = queue_for_cpu(raw_smp_processor_id(), QP_WRITE);

    return post_page(queue, folio, fastswap_offset(swap_offset), IB_WR_RDMA_WRITE) ?:
        wait_in_flight(queue) ?: (queue->is_failed ? -EIO : 0);
}

static int start_read(enum queue_type kind, pgoff_t swap_offset, struct folio *folio)
{
    struct fastswap_queue *queue = queue_for_cpu(raw_smp_processor_id(), kind);

    if (post_page(queue, folio, fastswap_offset(swap_offset), IB_WR_RDMA_READ)) {
        complete_folio(folio, false);
    }

    return 0;
}

static int rdma_load(unsigned swap_type, pgoff_t swap_offset, struct folio *folio)
{
    return start_read(QP_READ_SYNC, swap_offset, folio);
}

static int rdma_load_async(unsigned swap_type, pgoff_t swap_offset, struct folio *folio)
{
    return start_read(QP_READ_ASYNC, swap_offset, folio);
}

static int rdma_poll_load(int cpu)
{
    wait_in_flight(queue_for_cpu(cpu, QP_READ_SYNC));

    return 0;
}

static struct fastswap_ops rdma_ops = {
    .owner = THIS_MODULE,
    .store = rdma_store,
    .load = rdma_load,
    .load_async = rdma_load_async,
    .poll_load = rdma_poll_load,
};

/* other things init places the function into a seperate section instead of code section among */
static int __init fastswap_rdma_init(void)
{
    int i, ret = -EINVAL;

    if (queues <= 0) {
        // There may be a better way of doing this. We are trying not to hardcode the no of cpus.
        queues = num_online_cpus();
    }

    /* IPv4 only. Could be changed. 
     * init_net is the default sys namespace */
    ret = inet_pton_with_scope(&init_net, AF_INET, server_ip, server_port, &server_addr);
    if (!ret && client_ip[0]) {
        ret = inet_pton_with_scope(&init_net, AF_INET, client_ip, NULL, &client_addr);
    }

    if (ret) {
        pr_err("need a valid server_ip and server_port [, and a valid client_ip]\n");
        return ret;
    }

    /* GFP - get free paes */
    all_queues = kcalloc(QP_TYPES * queues, sizeof(*all_queues), GFP_KERNEL);

    if (!all_queues) {
        return -ENOMEM;
    }

    for (i = 0; i < QP_TYPES * queues; i++) {
        ret = connect_queue(&all_queues[i],
                i / queues == QP_READ_ASYNC ? IB_POLL_SOFTIRQ : IB_POLL_DIRECT);
        if (ret) {
            pr_err("queue %d: connect failed: %d\n", i, ret);
            close_queues();
            return ret;
        }
    }

    pr_info("remote base %llx size %llu key %u, %d queue sets\n",
            server_region.base_addr, server_region.size, server_region.remote_key, queues);

    ret = fastswap_register_ops(&rdma_ops);
    if (ret) {
        close_queues();
    }

    return ret;
}

/* rmmod only gets here with no far page left, so no read is in flight */
static void __exit fastswap_rdma_exit(void)
{
    int i;

    fastswap_unregister_ops(&rdma_ops);

    for (i = 0; i < QP_TYPES * queues; i++) {
        wait_in_flight(&all_queues[i]);
    }

    close_queues();
}

module_init(fastswap_rdma_init);
module_exit(fastswap_rdma_exit);
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("FastSwap RDMA backend");
