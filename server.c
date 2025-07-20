#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include "header.h"
#include <netinet/in.h>
#include <arpa/inet.h>


int main()
{
    // sare device le aao
    struct ibv_device **device_list;
    ibv_get_device_list(device_list);
    // device free kar lo if occupied
    ibv_free_device_list(device_list);
    struct ibv_context *context = ibv_open_device(device_list[0]);

    struct ibv_pd *pd = ibv_alloc_pd(context);

    // in case of failed operation NULL milega
    struct ibv_cq *cq = ibv_create_cq(context, 30, NULL, NULL, 0);
    char *buffer = malloc(1024);
    struct ibv_mr *mr = ibv_reg_mr(pd, buffer, 1024, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);

    struct ibv_qp_init_attr qp_attr = {
        .send_cq = cq, // why same
        .recv_cq = cq, // why same
        .cap = {
            .max_send_wr = 10,
            .max_recv_wr = 10,
            .max_send_sge = 5, // logic behind setting it to 5
            .max_recv_sge = 5  // logic behind setting it to 5

        },
        .qp_type = IBV_QPT_RC,
    };

    struct ibv_qp *qp = ibv_create_qp(pd, &qp_attr);
    struct rdma_event_channel *channel = rdma_create_event_channel();
    struct rdma_cm_id *cm_id;
    rdma_create_id(channel, &cm_id, NULL, RDMA_PS_TCP);
    struct sockaddr_in server_addr;

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(18515);                       // Choose your port
    inet_pton(AF_INET, "192.168.56.101", &server_addr.sin_addr); // Replace with your eth0's IP

    rdma_bind_addr(cm_id, (struct sockaddr *)&server_addr);
    rdma_listen(cm_id, 1);

    // aab memory ko deregister kar do
    // int a = ibv_dereg_mr(mr);
    // if (a != 0)
    // {
    //     printf("Failed to degegister memory error no. %d\n ", a);
    // }

    return 0;
}
