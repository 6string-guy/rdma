#ifndef RDMA_CALC_DEFS
#define RDMA_CALC_DEFS

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>
#include <stdint.h>

/* operation: 0 = add, 1 = sub, 2 = mul, 3 = div */
struct calc_req  { uint32_t a, b, op; };
struct calc_resp { uint32_t result;   };

#define BUF_SIZE 64   /* ≥ max(sizeof(req), sizeof(resp)) */

#endif
