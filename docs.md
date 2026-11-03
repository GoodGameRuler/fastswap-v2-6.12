# Some Kernel Breadcrumbs

Structures that `drivers/fastswap_rdma.c` touches. Definitions are from Linux 6.12 (`include/rdma/ib_verbs.h`, `include/rdma/rdma_cm.h`). 

## Connection (RDMA connection manager)

```c
struct rdma_cm_id {
    struct ib_device *device;        /* NIC the address resolved to; used for DMA mapping and ib_alloc_pd */
    void *context;                   /* pointer given to rdma_create_id; the driver stores its fastswap_queue here */
    struct ib_qp *qp;                /* queue pair made by rdma_create_qp; ib_post_send posts on it */
    rdma_cm_event_handler event_handler;  /* callback for connection events; connection_event_handler */
    enum rdma_ucm_port_space ps;     /* port space; RDMA_PS_TCP gives a reliable connected service */
    enum ib_qp_type qp_type;         /* QP type requested at creation; IB_QPT_RC */
    u32 port_num;                    /* NIC port in use */

    /* UNUSED */
    struct rdma_route route;
    struct work_struct net_work;
};

struct rdma_cm_event {
    enum rdma_cm_event_type event;   /* ADDR_RESOLVED, ROUTE_RESOLVED, ESTABLISHED, DISCONNECTED, ... */
    int status;                      /* 0, or the reason the step failed */
    union {
        struct rdma_conn_param conn; /* connection events: the peer's parameters and private data */
        struct rdma_ud_param ud;     /* datagram events; unused */
    } param;

    /* UNUSED */
    struct rdma_ucm_ece ece;
};

struct rdma_conn_param {
    const void *private_data;        /* bytes sent with the request; on ESTABLISHED, the server's reply (remote_region) */
    u8 private_data_len;             /* length of private_data; the driver rejects replies shorter than remote_region */
    u8 responder_resources;          /* RDMA reads the peer may have outstanding to this QP at once; 16 */
    u8 initiator_depth;              /* RDMA reads this QP may have outstanding to the peer at once; 16 */
    u8 flow_control;                 /* 1 enables end-to-end flow control */
    u8 retry_count;                  /* retries after a missing acknowledgement before the QP fails; 6 */
    u8 rnr_retry_count;              /* retries after "receiver not ready"; 7 means unlimited, never triggers here */

    /* UNUSED: ignored because the QP is created on the id */
    u8 srq;
    u32 qp_num;
    u32 qkey;
};
```

`connection_event_handler` returns 0 for every event. A non-zero return would make the connection manager destroy the id.

## Resources (protection domain, queue pair, completion queue)

```c
struct ib_pd {
    u32 local_dma_lkey;              /* key for local access to ordinary mapped memory; goes in every ib_sge */
    u32 flags;                       /* flags given to ib_alloc_pd; 0 */
    struct ib_device *device;        /* the device this PD belongs to */
    atomic_t usecnt;                 /* number of resources using the PD */
    u32 unsafe_global_rkey;          /* only set with IB_PD_UNSAFE_GLOBAL_RKEY; unused */

    /* UNUSED */
    struct ib_uobject *uobject;
    struct ib_mr *__internal_mr;
    struct rdma_restrack_entry res;
};

struct ib_qp_init_attr {
    void (*event_handler)(struct ib_event *, void *);  /* asynchronous QP events, in workqueue context; unset */
    void *qp_context;                /* second argument of event_handler; unset */
    struct ib_cq *send_cq;           /* completions of send-queue requests; set */
    struct ib_cq *recv_cq;           /* completions of receive-queue requests; set, same queue as send_cq */
    struct ib_srq *srq;              /* shared receive queue; if set, replaces the QP's own receive queue; unset */
    struct ib_xrcd *xrcd;            /* XRC target QPs only; unset */
    struct ib_qp_cap cap;            /* queue sizes; set */
    enum ib_sig_type sq_sig_type;    /* IB_SIGNAL_REQ_WR: only requests flagged IB_SEND_SIGNALED complete visibly */
    enum ib_qp_type qp_type;         /* IB_QPT_RC: reliable connection, required for RDMA read and write */
    u32 create_flags;                /* IB_QP_CREATE_* options; unset */
    u32 port_num;                    /* special QP types and the RDMA read/write helper API only; unset */
    struct ib_rwq_ind_table *rwq_ind_tbl;  /* spreads received packets over several receive queues; unset */
    u32 source_qpn;                  /* with IB_QP_CREATE_SOURCE_QPN, handled by the device driver; unset */
};

struct ib_qp_cap {
    u32 max_send_wr;                 /* most send-queue requests outstanding; QUEUE_DEPTH (reads and writes use the send queue) */
    u32 max_recv_wr;                 /* most receive-queue requests outstanding; 1, none are ever posted */
    u32 max_send_sge;                /* scatter-gather entries per send request; 1 */
    u32 max_recv_sge;                /* scatter-gather entries per receive request; 1 */
    u32 max_inline_data;             /* payload copied into the request itself; unset */
    u32 max_rdma_ctxs;               /* rdma_rw_ctx structures in flight, for the read/write helper API; unset */
};

struct ib_cq {
    struct ib_device *device;        /* device the queue lives on */
    void *cq_context;                /* private pointer given to ib_alloc_cq; the driver gets its fastswap_queue back */
    int cqe;                         /* capacity in entries, from nr_cqe */
    enum ib_poll_context poll_ctx;   /* how completions are collected, below */
    unsigned int comp_vector;        /* interrupt vector that signals new entries; 0 for every queue today */

    /* UNUSED */
    struct ib_ucq_object *uobject;
    ib_comp_handler comp_handler;
    void (*event_handler)(struct ib_event *, void *);
    unsigned int cqe_used;
    atomic_t usecnt;
    struct ib_wc *wc;
    struct list_head pool_entry;
    union {
        struct irq_poll iop;
        struct work_struct work;
    };
    struct workqueue_struct *comp_wq;
    struct dim *dim;
    ktime_t timestamp;
    u8 interrupt:1;
    u8 shared:1;
    struct rdma_restrack_entry res;
};

enum ib_poll_context {
    IB_POLL_SOFTIRQ,                 /* the core polls from softirq after the NIC interrupt; QP_READ_ASYNC */
    IB_POLL_WORKQUEUE,               /* the core polls from a workqueue; unused */
    IB_POLL_UNBOUND_WORKQUEUE,       /* as above, on an unbound workqueue; unused */
    IB_POLL_DIRECT,                  /* no interrupt; the caller polls with ib_process_cq_direct; QP_READ_SYNC and QP_WRITE */
};
```

## Posting a transfer

```c
struct ib_sge {
    u64 addr;                        /* local buffer as the NIC sees it; request->local_mapped_addr */
    u32 length;                      /* bytes to move; PAGE_SIZE */
    u32 lkey;                        /* local key that authorises the NIC to touch the buffer; protection_domain->local_dma_lkey */
};

struct ib_send_wr {
    struct ib_send_wr *next;         /* next request in a list posted together; NULL, one at a time */
    union {
        u64 wr_id;                   /* cookie returned in the completion; unused */
        struct ib_cqe *wr_cqe;       /* alternative cookie: the request's completion_callback; the core calls its done function */
    };
    struct ib_sge *sg_list;          /* array of local buffers */
    int num_sge;                     /* length of sg_list; 1 */
    enum ib_wr_opcode opcode;        /* IB_WR_RDMA_WRITE or IB_WR_RDMA_READ */
    int send_flags;                  /* IB_SEND_SIGNALED: produce a completion for this request */
    /* UNUSED */
    union {
        __be32 imm_data;
        u32 invalidate_rkey;
    } ex;
};

struct ib_rdma_wr {
    struct ib_send_wr wr;            /* the generic part, first so the two share an address; what ib_post_send takes */
    u64 remote_addr;                 /* address in the server's registered buffer; base_addr + remote_offset */
    u32 rkey;                        /* server's remote key; remote_key */
};
```

## Completion

```c
struct ib_cqe {
    void (*done)(struct ib_cq *cq, struct ib_wc *wc);  /* called by the core when the request completes; read_done or write_done */
};

struct ib_wc {
    union {
        u64 wr_id;
        struct ib_cqe *wr_cqe;       /* the completion_callback pointer from the work request; container_of recovers the request */
    };
    enum ib_wc_status status;        /* IB_WC_SUCCESS or the failure; any other value marks the queue failed */
    enum ib_wc_opcode opcode;        /* which operation finished */
    u32 vendor_err;                  /* device-specific error detail */
    u32 byte_len;                    /* bytes transferred; valid for reads, not writes */
    struct ib_qp *qp;                /* QP the request ran on */

    /* UNUSED */
    union {
        __be32 imm_data;
        u32 invalidate_rkey;
    } ex;
    u32 src_qp;
    u32 slid;
    int wc_flags;
    u16 pkey_index;
    u8 sl;
    u8 dlid_path_bits;
    u32 port_num;
    u8 smac[ETH_ALEN];
    u16 vlan_id;
    u8 network_hdr_type;
};
```
