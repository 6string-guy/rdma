#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include<rdma/rdma_cma.h>


int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <a> <b> <op>\n"
                "  op: 0=add,1=sub,2=mul,3=div\n", argv[0]);
        return 1;
    }

    uint32_t a = atoi(argv[1]), b = atoi(argv[2]), op = atoi(argv[3]);

    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *cm_id;
    rdma_create_id(ec, &cm_id, NULL, RDMA_PS_TCP);

    // Resolve server address
    struct sockaddr_in server_addr = {
      .sin_family = AF_INET,
      .sin_port   = htons(18515)
    };
    inet_pton(AF_INET, "192.168.56.103", &server_addr.sin_addr);
    rdma_resolve_addr(cm_id, NULL,
                      (struct sockaddr *)&server_addr, 2000);
    struct rdma_cm_event *event;
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);

    // Resolve route
    rdma_resolve_route(cm_id, 2000);
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);

    // Allocate RDMA resources
    struct ibv_pd *pd = ibv_alloc_pd(cm_id->verbs);
    struct ibv_cq *cq = ibv_create_cq(cm_id->verbs, 1, NULL, NULL, 0);
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap     = { .max_send_wr = 1, .max_recv_wr = 1,
                     .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    rdma_create_qp(cm_id, pd, &qp_attr);

    // Allocate and register buffer
    void *buf = malloc(BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Connect
    struct rdma_conn_param conn_param = { 0 };
    rdma_connect(cm_id, &conn_param);
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);
    printf("Connected to server\n");

    // Prepare and send request
    struct calc_req req = {
      htonl(a), htonl(b), htonl(op)
    };
    memcpy(buf, &req, sizeof(req));
    struct ibv_sge sge = {
      .addr = (uintptr_t)buf,
      .length = sizeof(req),
      .lkey = mr->lkey
    };
    struct ibv_send_wr send_wr = {
      .wr_id = 1, .sg_list = &sge, .num_sge = 1,
      .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED
    };
    struct ibv_send_wr *bad_send;
    ibv_post_send(cm_id->qp, &send_wr, &bad_send);

    // Poll for send completion
    struct ibv_wc wc;
    do {
      ibv_poll_cq(cq, 1, &wc);
    } while (wc.status == IBV_WC_SUCCESS && wc.opcode != IBV_WC_SEND);

    // Post receive for response
    struct ibv_recv_wr recv_wr = {
      .wr_id = 2, .sg_list = &sge, .num_sge = 1
    }, *bad_recv;
    ibv_post_recv(cm_id->qp, &recv_wr, &bad_recv);

    // Poll for response
    do {
      ibv_poll_cq(cq, 1, &wc);
    } while (wc.status == IBV_WC_SUCCESS && wc.opcode != IBV_WC_RECV);

    struct calc_resp *resp = buf;
    uint32_t result = ntohl(resp->result);
    printf("Result from server: %u\n", result);

    // Cleanup
    rdma_disconnect(cm_id);
    ibv_dereg_mr(mr);
    free(buf);
    rdma_destroy_qp(cm_id);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(cm_id);
    rdma_destroy_event_channel(ec);
    return 0;
}
