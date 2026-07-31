#ifndef _NVMERGE_H_
#define _NVMERGE_H_

#include <linux/types.h>

#define INVALID_ID 0xffff

typedef struct {
    u32 id;
    char name[64];
} nv_cfg_t;

extern nv_cfg_t nv_cfg[];

BOOLEAN mergeItem(u8 *oldBuf, u32 oldNVlength, u8 *newBuf, u32 newNVlength);

#endif /* _NVMERGE_H_ */
