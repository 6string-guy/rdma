#ifndef RDMA_CALC_HEADER_H
#define RDMA_CALC_HEADER_H

#include <stdint.h>
#include <endian.h>     /* htobe32 / be32toh fall-backs */
#include <rdma/rdma_cma.h>

/* ---------- defaults ---------- */
#define DEFAULT_PORT    18515
#define DEFAULT_IP_SRV  "0.0.0.0"   /* listen on all ifaces                */
#define DEFAULT_IP_CLI  "127.0.0.1" /* client connects here if not given   */
#define MAX_PENDING     10          /* backlog for rdma_listen()           */
#define BUF_SIZE        1024        /* plenty for tiny structs             */

/* ---------- message formats ---------- */
struct calc_req  { uint32_t a; uint32_t b; uint32_t op;     }; /* host byte order always network */
struct calc_resp { uint32_t result;                          };

enum { OP_ADD = 0, OP_SUB = 1, OP_MUL = 2, OP_DIV = 3, OP_QUIT = 0x7FFFFFFF };

static inline uint32_t to_net (uint32_t h) { return htobe32(h); }
static inline uint32_t to_host(uint32_t n) { return be32toh(n); }

#endif /* RDMA_CALC_HEADER_H */
