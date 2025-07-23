/* interactive RDMA calculator client */
#include "header.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>

static volatile sig_atomic_t running = 1;
static void sigint_handler(int){ running = 0; }

static int poll_one(struct ibv_cq *cq, enum ibv_wc_opcode expect, struct ibv_wc *wc)
{
    for (;;){
        int n = ibv_poll_cq(cq, 1, wc);
        if (n < 0) return -1;
        if (n == 0) continue;
        if (wc->status != IBV_WC_SUCCESS) return -1;
        return (wc->opcode == expect) ? 0 : -1;
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-i server_ip] [-p port]\n"
        "  -i ip    server IP to connect (default "DEFAULT_IP_CLI")\n"
        "  -p port  server port          (default %d)\n", prog, DEFAULT_PORT);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    struct sigaction sa = { .sa_handler=sigint_handler };
    sigaction(SIGINT, &sa, NULL);

    const char *srv_ip = DEFAULT_IP_CLI;
    int port = DEFAULT_PORT;
    int opt;
    while ((opt = getopt(argc, argv, "i:p:h")) != -1){
        switch(opt){
            case 'i': srv_ip = optarg;     break;
            case 'p': port   = atoi(optarg); break;
            default : usage(argv[0]);
        }
    }

    struct rdma_event_channel *ec = rdma_create_event_channel();
    struct rdma_cm_id *id;
    rdma_create_id(ec, &id, NULL, RDMA_PS_TCP);

    struct sockaddr_in dst = { .sin_family=AF_INET,
                               .sin_port  = htons(port) };
    if (inet_pton(AF_INET, srv_ip, &dst.sin_addr) != 1){
        perror("inet_pton"); return EXIT_FAILURE;
    }
    printf("Connecting to %s:%d...\n", srv_ip, port);

    rdma_resolve_addr(id, NULL, (struct sockaddr*)&dst, 2000);
    struct rdma_cm_event *ev;
    rdma_get_cm_event(ec,&ev); rdma_ack_cm_event(ev);
    rdma_resolve_route(id, 2000);
    rdma_get_cm_event(ec,&ev); rdma_ack_cm_event(ev);

    struct ibv_pd *pd = ibv_alloc_pd(id->verbs);
    struct ibv_cq *cq = ibv_create_cq(id->verbs, 4, NULL, NULL, 0);
    struct ibv_qp_init_attr qattr = {
        .send_cq=cq, .recv_cq=cq,
        .cap = { .max_send_wr=2,.max_recv_wr=2,.max_send_sge=1,.max_recv_sge=1 },
        .qp_type=IBV_QPT_RC
    };
    rdma_create_qp(id,pd,&qattr);

    void *buf = calloc(1, BUF_SIZE);
    struct ibv_mr *mr = ibv_reg_mr(pd, buf, BUF_SIZE,
                                   IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    struct rdma_conn_param cp={0};
    rdma_connect(id,&cp);
    rdma_get_cm_event(ec,&ev); rdma_ack_cm_event(ev);
    puts("Connected. Type \"help\" for commands.");

    char line[128];
    while (running){
        printf("calc> "); fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;
        if (!running) break;

        /* trim newline */
        line[strcspn(line,"\n")] = 0;

        if (strcmp(line,"help")==0 || strcmp(line,"?")==0){
            puts("Syntax: <a> <b> <op>\n"
                 "  op: 0=add 1=sub 2=mul 3=div\n"
                 "Type \"exit\" to quit.");
            continue;
        }
        if (strcmp(line,"exit")==0 || strcmp(line,"quit")==0){
            struct calc_req req = { 0,0,to_net(OP_QUIT) };
            memcpy(buf,&req,sizeof req);
            struct ibv_sge sge = { .addr=(uintptr_t)buf, .length=sizeof req, .lkey=mr->lkey };
            struct ibv_send_wr swr = { .wr_id=1, .sg_list=&sge, .num_sge=1,
                                       .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED };
            struct ibv_send_wr *bad_s;
            ibv_post_send(id->qp,&swr,&bad_s);
            struct ibv_wc wc;
            poll_one(cq,IBV_WC_SEND,&wc);
            break;
        }

        int32_t a,b,op;
        if (sscanf(line,"%d %d %d",&a,&b,&op)!=3 || op<0 || op>3){
            puts("Bad input. Type \"help\".");
            continue;
        }

        /* prepare receive for response */
        struct ibv_sge sge_r = { .addr=(uintptr_t)buf, .length=sizeof(struct calc_resp), .lkey=mr->lkey };
        struct ibv_recv_wr rwr = { .wr_id=2, .sg_list=&sge_r, .num_sge=1 };
        struct ibv_recv_wr *bad_r;
        ibv_post_recv(id->qp,&rwr,&bad_r);

        struct calc_req req = { to_net(a), to_net(b), to_net(op) };
        memcpy(buf,&req,sizeof req);

        struct ibv_sge sge_s = { .addr=(uintptr_t)buf, .length=sizeof req, .lkey=mr->lkey };
        struct ibv_send_wr swr = { .wr_id=1, .sg_list=&sge_s, .num_sge=1,
                                   .opcode=IBV_WR_SEND, .send_flags=IBV_SEND_SIGNALED };
        struct ibv_send_wr *bad_s;
        ibv_post_send(id->qp,&swr,&bad_s);

        struct ibv_wc wc;
        if (poll_one(cq,IBV_WC_SEND,&wc)<0) { puts("Send error"); break; }
        if (poll_one(cq,IBV_WC_RECV,&wc)<0) { puts("Recv error"); break; }

        struct calc_resp *resp = buf;
        printf("Result: %d\n", (int32_t)to_host(resp->result));
    }

    rdma_disconnect(id);
    ibv_dereg_mr(mr); free(buf);
    rdma_destroy_qp(id); ibv_destroy_cq(cq); ibv_dealloc_pd(pd);
    rdma_destroy_id(id); rdma_destroy_event_channel(ec);
    puts("Client closed.");
    return 0;
}
