#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

int main() {
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id;
    rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);

    // Bind to SoftRoCE interface IP and port
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(18515)
    };
    inet_pton(AF_INET, "192.168.56.103", &addr.sin_addr);
    rdma_bind_addr(listen_id, (struct sockaddr *)&addr);
    rdma_listen(listen_id, 1);
    printf("Server listening on %s:%d\n", "192.168.56.103", 18515);

    // Wait for a connection request
    struct rdma_cm_event *event;
    rdma_get_cm_event(ec, &event);
    struct rdma_cm_id *conn_id = event->id;
    rdma_ack_cm_event(event);

    // Allocate RDMA resources
    struct ibv_pd *pd = ibv_alloc_pd(conn_id->verbs);
    struct ibv_cq *cq = ibv_create_cq(conn_id->verbs, 1, NULL, NULL, 0);
    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq, .recv_cq = cq,
        .cap     = { .max_send_wr = 1, .max_recv_wr = 1,
                     .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type = IBV_QPT_RC
    };
    rdma_create_qp(conn_id, pd, &qp_attr);

    // Allocate and register buffer
    void *buf = malloc(BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Accept the connection
    struct rdma_conn_param conn_param = { 0 };
    rdma_accept(conn_id, &conn_param);
    rdma_get_cm_event(ec, &event);
    rdma_ack_cm_event(event);
    printf("Client connected\n");

    // Post a receive for the incoming request
    struct ibv_sge sge = { .addr = (uintptr_t)buf,
                           .length = sizeof(struct calc_req),
                           .lkey = mr->lkey };
    struct ibv_recv_wr recv_wr = { .wr_id = 1, .sg_list = &sge,
                                   .num_sge = 1 };
    struct ibv_recv_wr *bad_recv;
    ibv_post_recv(conn_id->qp, &recv_wr, &bad_recv);

    // Poll for the request
    struct ibv_wc wc;
    do {
       ibv_poll_cq(cq, 1, &wc);
    } while (wc.status == IBV_WC_SUCCESS && wc.opcode != IBV_WC_RECV);

    struct calc_req *req = buf;
    uint32_t a = ntohl(req->a), b = ntohl(req->b), op = ntohl(req->op);
    uint32_t result = 0;
    switch (op) {
      case 0: result = a + b; break;
      case 1: result = a - b; break;
      case 2: result = a * b; break;
      case 3: result = (b!=0)? a / b : 0; break;
      default: result = 0;
    }
    printf("Computed: %u %s %u = %u\n", a,
           (op==0?"+":op==1?"-":op==2?"*":"/"), b, result);

    // Prepare and send the response
    struct calc_resp resp = { htonl(result) };
    memcpy(buf, &resp, sizeof(resp));
    sge.length = sizeof(resp);
    struct ibv_send_wr send_wr = {
      .wr_id = 2, .sg_list = &sge, .num_sge = 1,
      .opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED
    };
    struct ibv_send_wr *bad_send;
    ibv_post_send(conn_id->qp, &send_wr, &bad_send);

    // Poll for send completion
    do {
       ibv_poll_cq(cq, 1, &wc);
    } while (wc.status == IBV_WC_SUCCESS && wc.opcode != IBV_WC_SEND);

    printf("Response sent\n");

    // Cleanup
    rdma_disconnect(conn_id);
    ibv_dereg_mr(mr);
    free(buf);
    rdma_destroy_qp(conn_id);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    rdma_destroy_id(conn_id);
    rdma_destroy_event_channel(ec);
    return 0;
}
