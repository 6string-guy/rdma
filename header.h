// rdma_calc.h
#ifndef RDMA_CALC_H
#define RDMA_CALC_H

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>

// Request: two operands and an opcode (0=add,1=sub,2=mul,3=div)
struct calc_req {
    uint32_t a;
    uint32_t b;
    uint32_t op;
};

// Response: single 32-bit result
struct calc_resp {
    uint32_t result;
};

// Buffer size large enough for both request and response
#define BUF_SIZE (sizeof(struct calc_req) > sizeof(struct calc_resp) ? \
                  sizeof(struct calc_req) : sizeof(struct calc_resp))

#endif // RDMA_CALC_H
