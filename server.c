#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

static int poll_one(struct ibv_cq *cq, enum ibv_wc_opcode expect, struct ibv_wc *wc) {
    while (1) {
        int n = ibv_poll_cq(cq, 1, wc);
        if (n < 0)  return -1;
        if (n == 0) continue;
        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr,"CQ Error: status=%d, opcode=%d\n", wc->status, wc->opcode);
            return -1;
        }
        if (wc->opcode == expect) return 0;
    }
}

int main(void)
{
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id;
    rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(18515) };
    inet_pton(AF_INET, "192.168.56.103", &addr.sin_addr);
    rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
    rdma_listen(listen_id, 1);
    printf("Server listening on 192.168.56.103:18515\n");

    while (1) {
        struct rdma_cm_event *ev;
        rdma_get_cm_event(ec, &ev); // BLOCKS until client connects
        struct rdma_cm_id *id = ev->id;
        rdma_ack_cm_event(ev);

        struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
        struct ibv_cq *cq = ibv_create_cq(id->verbs, 2, NULL, NULL, 0);
        struct ibv_qp_init_attr qattr = {
            .send_cq = cq, .recv_cq = cq,
            .cap     = { .max_send_wr = 1, .max_recv_wr = 1,
                         .max_send_sge = 1, .max_recv_sge = 1 },
            .qp_type = IBV_QPT_RC
        };
        rdma_create_qp(id, pd, &qattr);

        void *buf = calloc(1, BUF_SIZE);
        struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

        struct rdma_conn_param cp = {0};
        rdma_accept(id, &cp);
        rdma_get_cm_event(ec, &ev); rdma_ack_cm_event(ev);
        printf("Client connected\n");

        // --- Process multiple requests from this client
        int running = 1;
        while (running) {
            // Post receive
            struct ibv_sge sge = { .addr=(uintptr_t)buf, .length=sizeof(struct calc_req), .lkey=mr->lkey };
            struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&sge, .num_sge=1 };
            struct ibv_recv_wr *bad;
            ibv_post_recv(id->qp, &rwr, &bad);

            struct ibv_wc wc;
            if (poll_one(cq, IBV_WC_RECV, &wc)) { fprintf(stderr,"recv wc error\n"); break; }

            struct calc_req *req = buf;
            uint32_t A = ntohl(req->a), B = ntohl(req->b), op = ntohl(req->op);
            // A special case: detect shutdown (client sends op==0xFFFFFFFF)
            if(op == 0xFFFFFFFF) {
                printf("Client requested disconnect. Closing connection.\n");
                running = 0;
                continue;
            }

            uint32_t res = 0;
            switch (op) { case 0: res=A+B; break; case 1: res=A-B; break;
                          case 2: res=A*B; break; case 3: res=B?A/B:0; break; }
            printf("Computed: %u %s %u = %u\n",A,op==0?"+":op==1?"-":op==2?"*":"/",B,res);

            *((struct calc_resp *)buf) = (struct calc_resp){ htonl(res) };
            sge.length = sizeof(struct calc_resp);
            struct ibv_send_wr swr = { .wr_id=2, .sg_list=&sge, .num_sge=1,
                                       .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED };
            ibv_post_send(id->qp, &swr, &bad);
            if (poll_one(cq, IBV_WC_SEND, &wc)) { fprintf(stderr,"send wc error\n"); break; }
        }

        // Cleanup this client's resources
        rdma_disconnect(id);
        ibv_dereg_mr(mr); free(buf);
        rdma_destroy_qp(id); ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
        rdma_destroy_id(id);
        printf("Client disconnected.\n");
    }
    rdma_destroy_id(listen_id); rdma_destroy_event_channel(ec);
    return 0;
}
