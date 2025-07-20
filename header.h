#include<infiniband/verbs.h>
#include<stdio.h>
#include<stdlib.h>

#define CQ_CAPACITY (20)
#define MAX_SGE (5)
#define MAX_WR (15)
#define DEFAULT_RDMA_PORT (20886)


struct rdma_buffer_attr
{
    uint64_t address;
    uint32_t length;
    union stag {
        uint32_t local_stag;
        uint32_t remote_stag;
    } stag;
    
};


