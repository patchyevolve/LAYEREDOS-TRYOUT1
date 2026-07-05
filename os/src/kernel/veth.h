#ifndef VETH_H
#define VETH_H

#include "types.h"
#include "eth.h"

#define VETH_MAX_PAIRS   8
#define VETH_MTU         ETH_MAX_FRAME

/* Forward declaration */
struct net_ns;

typedef struct veth_end {
    uint8_t           mac[ETH_ALEN];
    struct veth_end*  peer;
    struct net_ns*    ns;
    int               used;
} veth_end_t;

typedef struct {
    veth_end_t a;
    veth_end_t b;
    int        used;
    char       name[16];
} veth_pair_t;

void veth_init(void);

/* Find the veth end with a given MAC. Returns NULL if not found. */
veth_end_t* veth_find_end(const uint8_t* mac);

/* Deliver a raw Ethernet frame payload to a veth end.
 * The frame is dispatched in the end's namespace. */
err_t veth_deliver(veth_end_t* end, const uint8_t* src_mac, uint16_t type,
                   const uint8_t* data, uint32_t len);

/* Create a veth pair with auto-generated MACs.
 * Both ends are placed in the current namespace.
 * Returns 0 on success. Returns two end indices in out_a, out_b. */
err_t veth_pair_create(int* out_idx);

/* Move one end of a veth pair to a different namespace.
 * pair_idx: index from veth_pair_create
 * end_sel: 0 for end A, 1 for end B
 * target_ns: the target namespace */
err_t veth_end_move(int pair_idx, int end_sel, struct net_ns* target_ns);

/* Get the MAC of a veth end by pair/end */
const uint8_t* veth_get_mac(int pair_idx, int end_sel);

#endif
