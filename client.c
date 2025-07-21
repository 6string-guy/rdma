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

int main() {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *id;
    rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);
    struct sockaddr_in dst = { .sin_family = AF_INET, .sin_port = htons(18515) };
    inet_pton(AF_INET, "192.168.56.103", &dst.sin_addr);

    rdma_resolve_addr(id, NULL, (struct sockaddr*)&dst, 2000);
    struct rdma_cm_event *ev;
    rdma_get_cm_event(ec, &ev); rdma_ack_cm_event(ev);
    rdma_resolve_route(id, 2000);
    rdma_get_cm_event(ec, &ev); rdma_ack_cm_event(ev);

    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    struct ibv_cq *cq = ibv_create_cq(id->verbs,2,NULL,NULL,0);
    struct ibv_qp_init_attr qattr = {
        .send_cq=cq, .recv_cq=cq,
        .cap={ .max_send_wr=1,.max_recv_wr=1,.max_send_sge=1,.max_recv_sge=1 },
        .qp_type=IBV_QPT_RC
    };
    rdma_create_qp(id,pd,&qattr);

    void *buf = calloc(1,BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd,buf,BUF_SIZE,
                       IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE);

    struct rdma_conn_param cp={0};
    rdma_connect(id,&cp);
    rdma_get_cm_event(ec,&ev); rdma_ack_cm_event(ev);
    puts("Connected");

    char input[100];
    while (1) {
        printf("Enter: <a> <b> <op> (0=add 1=sub 2=mul 3=div) or 'exit'\n> ");
        fflush(stdout);
        if (!fgets(input, sizeof input, stdin)) break;

        if (strncmp(input, "exit", 4) == 0) {
            struct calc_req req = {0,0,htonl(0x7FFFFFFF)};
            struct ibv_sge sge = { .addr=(uintptr_t)buf, .length=sizeof(req), .lkey=mr->lkey };
            memcpy(buf,&req,sizeof(req));
            struct ibv_send_wr swr = { .wr_id=1, .sg_list=&sge, .num_sge=1, .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED };
            struct ibv_send_wr *bad;
            ibv_post_send(id->qp, &swr, &bad);
            struct ibv_wc wc;
            poll_one(cq, IBV_WC_SEND, &wc);
            break;
        }

        int32_t A,B,op;
        if (sscanf(input, "%d %d %d", &A, &B, &op) != 3 || op<0 || op>3) {
            printf("Bad input, try again.\n");
            continue;
        }
        struct ibv_sge sge_resp = { .addr=(uintptr_t)buf, .length=sizeof(struct calc_resp), .lkey=mr->lkey };
        struct ibv_recv_wr rwr={ .wr_id=2,.sg_list=&sge_resp,.num_sge=1 },*bad;
        ibv_post_recv(id->qp,&rwr,&bad);

        struct calc_req req = { htonl(A), htonl(B), htonl(op) };
        struct ibv_sge sge_req = { .addr=(uintptr_t)buf, .length=sizeof(req), .lkey=mr->lkey };
        memcpy(buf,&req,sizeof(req));
        struct ibv_send_wr swr = { .wr_id=1,.sg_list=&sge_req,.num_sge=1,.opcode=IBV_WR_SEND,.send_flags=IBV_SEND_SIGNALED };
        ibv_post_send(id->qp,&swr,&bad);

        struct ibv_wc wc;
        if (poll_one(cq,IBV_WC_SEND,&wc)) { printf("send wc error\n"); break; }
        if (poll_one(cq,IBV_WC_RECV,&wc)) { printf("recv wc error\n"); break; }
        int32_t res = ntohl(((struct calc_resp*)buf)->result);
        printf("Result: %d\n",res);
    }

    rdma_disconnect(id);
    ibv_dereg_mr(mr); free(buf);
    rdma_destroy_qp(id); ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
    rdma_destroy_id(id); rdma_destroy_event_channel(ec);
    puts("Client closed.");
    return 0;
}
