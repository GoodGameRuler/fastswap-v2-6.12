#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <rdma/rdma_cma.h>

struct memregion {
    uint64_t baseaddr;
    uint64_t size;
    uint32_t key;
    uint32_t reserved;
};

static int fail(const char *what)
{
    perror(what);
    return 1;
}

static int accept_client(struct rdma_cm_id *id, struct rdma_conn_param *request,
        struct ibv_pd *pd, struct memregion *region)
{
    struct ibv_qp_init_attr attr = {
        .qp_type = IBV_QPT_RC,
        .cap = { .max_send_wr = 10, .max_recv_wr = 10, .max_send_sge = 1, .max_recv_sge = 1 },
    };

    struct rdma_conn_param reply = {
        .private_data = region,
        .private_data_len = sizeof(*region),
        .initiator_depth = request->responder_resources,
        .responder_resources = request->initiator_depth,
        .rnr_retry_count = request->rnr_retry_count,
    };

    if (rdma_create_qp(id, pd, &attr)) {
        return fail("rdma_create_qp");
    }

    if (rdma_accept(id, &reply)) {
        rdma_destroy_qp(id);
        return fail("rdma_accept");
    }

    return 0;
}

int main(int argc, char **argv)
{
    long port = argc == 3 ? strtol(argv[1], NULL, 10) : -1;
    long size_mb = argc == 3 ? strtol(argv[2], NULL, 10) : -1;

    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port),
        .sin_addr.s_addr = htonl(INADDR_ANY) };
    struct rdma_event_channel *channel;
    struct rdma_cm_id *listener;
    struct memregion region = { .size = (uint64_t)size_mb << 20 };
    struct ibv_pd *pd = NULL;
    struct ibv_mr *mr = NULL;

    void *buffer = NULL;

    if (port < 1 || port > 65535 || size_mb < 1) {
        fprintf(stderr, "usage: %s <port> <size_mb>\n", argv[0]);
        return 1;
    }

    if (!(channel = rdma_create_event_channel())) {
        return fail("rdma_create_event_channel");
    }

    if (rdma_create_id(channel, &listener, NULL, RDMA_PS_TCP)) {
        fail("rdma_create_id");
        goto out_channel;
    }

    if (rdma_bind_addr(listener, (struct sockaddr *)&addr) || rdma_listen(listener, 64)) {
        fail("rdma_bind_addr/rdma_listen");
        goto out;
    }

    printf("listening on port %ld\n", port);

    for (;;) {
        struct rdma_cm_event *ev;
        struct rdma_cm_event e;

        if (rdma_get_cm_event(channel, &ev)) {
            fail("rdma_get_cm_event");
            goto out;
        }

        e = *ev;
        rdma_ack_cm_event(ev);

        switch (e.event) {
            case RDMA_CM_EVENT_CONNECT_REQUEST:
                if (!pd) {
                    if (!(pd = ibv_alloc_pd(e.id->verbs))) {
                        fail("ibv_alloc_pd");
                        goto out;
                    }

                    if (!(buffer = malloc(region.size))) {
                        fail("malloc");
                        goto out;
                    }

                    mr = ibv_reg_mr(pd, buffer, region.size,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
                    if (!mr) {
                        fail("ibv_reg_mr");
                        goto out;
                    }

                    region.baseaddr = (uintptr_t)buffer;
                    region.key = mr->rkey;
                }
                if (accept_client(e.id, &e.param.conn, pd, &region)) {
                    rdma_reject(e.id, NULL, 0);
                    rdma_destroy_id(e.id);
                }
                break;

            case RDMA_CM_EVENT_ESTABLISHED:
            case RDMA_CM_EVENT_TIMEWAIT_EXIT:
                break;

            case RDMA_CM_EVENT_DISCONNECTED:
            case RDMA_CM_EVENT_CONNECT_ERROR:
            case RDMA_CM_EVENT_REJECTED:
            case RDMA_CM_EVENT_UNREACHABLE:
                fprintf(stderr, "closing connection: %s\n", rdma_event_str(e.event));
                rdma_destroy_qp(e.id);
                rdma_destroy_id(e.id);
                break;

            default:
                fprintf(stderr, "ignoring event: %s\n", rdma_event_str(e.event));
        }
    }

out:
    if (mr) {
        ibv_dereg_mr(mr);
    }

    free(buffer);

    if (pd) {
        ibv_dealloc_pd(pd);
    }

    rdma_destroy_id(listener);

out_channel:
    rdma_destroy_event_channel(channel);

    return 1;
}
