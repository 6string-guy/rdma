/* multithreaded RDMA calculator server */
#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <arpa/inet.h>

/* ---------- helpers ---------- */
static volatile sig_atomic_t server_running = 1;
static void sigint_handler(int){ server_running = 0; }

/* poll exactly one CQ entry of expected opcode */
static int poll_one(struct ibv_cq *cq, enum ibv_wc_opcode expect, struct ibv_wc *wc)
{
    for (;;) {
        int n = ibv_poll_cq(cq, 1, wc);
        if (n < 0) return -1;
        if (n == 0) continue;
        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "CQ error: status=%d, opcode=%d\n", wc->status, wc->opcode);
            return -1;
        }
        return (wc->opcode == expect) ? 0 : -1;
    }
}

/* ---------- per-client worker ---------- */
struct worker_arg { struct rdma_cm_id *id; };

static void *client_worker(void *arg)
{
    struct rdma_cm_id *id = ((struct worker_arg*)arg)->id;
    free(arg);

    /* Allocate RDMA resources */
    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    struct ibv_cq *cq = ibv_create_cq(id->verbs, 4, NULL, NULL, 0);
    struct ibv_qp_init_attr qattr = {
        .send_cq = cq, .recv_cq = cq,
        .cap      = { .max_send_wr = 2, .max_recv_wr = 2,
                      .max_send_sge = 1, .max_recv_sge = 1 },
        .qp_type  = IBV_QPT_RC
    };
    rdma_create_qp(id, pd, &qattr);

    void *buf = calloc(1, BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    struct rdma_conn_param cp = {0};
    rdma_accept(id, &cp);

    /* wait for ESTABLISHED */
    struct rdma_event_channel *ec = id->channel;
    struct rdma_cm_event *ev;
    rdma_get_cm_event(ec, &ev);
    rdma_ack_cm_event(ev);

    printf("[+] client %p connected\n", id);

    for (;;) {
        /* post receive for request */
        struct ibv_sge sge_r = { .addr=(uintptr_t)buf, .length=sizeof(struct calc_req), .lkey=mr->lkey };
        struct ibv_recv_wr rwr = { .wr_id=1, .sg_list=&sge_r, .num_sge=1 };
        struct ibv_recv_wr *bad_r;
        ibv_post_recv(id->qp, &rwr, &bad_r);

        struct ibv_wc wc;
        if (poll_one(cq, IBV_WC_RECV, &wc) != 0) break;

        struct calc_req *req = buf;
        uint32_t op = to_host(req->op);
        if (op == OP_QUIT || !server_running) break;

        int32_t a = (int32_t)to_host(req->a);
        int32_t b = (int32_t)to_host(req->b);
        int32_t res = 0;
        switch (op){
            case OP_ADD: res = a + b;            break;
            case OP_SUB: res = a - b;            break;
            case OP_MUL: res = a * b;            break;
            case OP_DIV: res = b ? a / b : 0;    break;
            default:     res = 0;
        }
        printf("[client %p] %d %c %d = %d\n", id, a,
               op==OP_ADD?'+':op==OP_SUB?'-':op==OP_MUL?'*':'/', b, res);

        struct calc_resp *resp = (struct calc_resp*)buf;
        resp->result = to_net(res);

        struct ibv_sge sge_s = { .addr=(uintptr_t)buf, .length=sizeof(*resp), .lkey=mr->lkey };
        struct ibv_send_wr swr = { .wr_id=2, .sg_list=&sge_s, .num_sge=1,
                                   .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED };
        struct ibv_send_wr *bad_s;
        ibv_post_send(id->qp, &swr, &bad_s);
        if (poll_one(cq, IBV_WC_SEND, &wc) != 0) break;
    }

    rdma_disconnect(id);
    ibv_dereg_mr(mr); free(buf);
    rdma_destroy_qp(id); ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
    rdma_destroy_id(id);

    printf("[-] client %p disconnected\n", id);
    return NULL;
}

/* ---------- main ---------- */
static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-i ip] [-p port]\n"
        "  -i ip    IP address to bind (default "DEFAULT_IP_SRV")\n"
        "  -p port  port to listen      (default %d)\n", prog, DEFAULT_PORT);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    /* install Ctrl+C handler */
    struct sigaction sa = { .sa_handler=sigint_handler };
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM,&sa, NULL);

    const char *ip = DEFAULT_IP_SRV;
    int port       = DEFAULT_PORT;

    int opt;
    while ((opt = getopt(argc, argv, "i:p:h")) != -1) {
        switch (opt){
            case 'i': ip   = optarg;         break;
            case 'p': port = atoi(optarg);   break;
            default : usage(argv[0]);
        }
    }

    /* RDMA CM setup */
    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *listen_id;
    rdma_create_id(ec, &listen_id, NULL, RDMA_PS_TCP);

    struct sockaddr_in addr = { .sin_family=AF_INET,
                                .sin_port  = htons(port) };
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1){
        perror("inet_pton"); return EXIT_FAILURE;
    }

    if (rdma_bind_addr(listen_id, (struct sockaddr*)&addr)){
        perror("rdma_bind_addr"); return EXIT_FAILURE;
    }
    if (rdma_listen(listen_id, MAX_PENDING)){
        perror("rdma_listen"); return EXIT_FAILURE;
    }
    printf("RDMA server listening on %s:%d (Ctrl+C to stop)\n", ip, port);

    /* main accept loop */
    while (server_running){
        struct rdma_cm_event *ev;
        if (rdma_get_cm_event(ec, &ev)) continue;
        if (ev->event == RDMA_CM_EVENT_CONNECT_REQUEST){
            struct worker_arg *wa = malloc(sizeof *wa);
            wa->id = ev->id;
            pthread_t tid;
            pthread_create(&tid, NULL, client_worker, wa);
            pthread_detach(tid);
        }
        rdma_ack_cm_event(ev);
    }

    rdma_destroy_id(listen_id);
    rdma_destroy_event_channel(ec);
    printf("Server shut down.\n");
    return 0;
}
