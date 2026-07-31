#include <common.h>
#include <malloc.h>
#include <linux/input.h>
#include "sparse_format.h"
#include <dl_common.h>
#include "nvmerge.h"
#include "dl_operate.h"
#ifdef  NVMERGE_TRACE
#undef  NVMERGE_TRACE
#endif // NVMERGE_TRACE

#define  NVMERGE_TRACE debugf
/*
    id:
    nvBuf:        nv data
    nvLength:    nv size
    itemPos:    item pos
    itemSize:    item size
*/
static BOOLEAN ___findItem( /*IN*/ u32 id, /*IN*/ u8 * nvBuf, /*IN*/ u32 nvLength, /*OUT*/ u32 * itemSize, /*OUT*/ u32 * itemPos)
{
    u32 offset = 4;
    u16 tmp[2];
    while (1) {
        if (*(u16 *) (nvBuf + offset) == INVALID_ID) {
            NVMERGE_TRACE("NVMERGE:___findItem find the tail\n");
            break;
        }
        if (offset + sizeof(tmp) > nvLength) {
            NVMERGE_TRACE("NVMERGE: ___findItem Surpass the boundary of the part\r\n");
            break;
        }
        memcpy(tmp, nvBuf + offset, sizeof(tmp));
        offset += sizeof(tmp);
        if (id == (u32) tmp[0]) {
            *itemSize = (u32) tmp[1];
            *itemPos = offset;
            NVMERGE_TRACE("NVMERGE:___findItem id = 0x%x\n", id);
            return TRUE;
        }
        offset += tmp[1];
        offset = (offset + 3) & 0xFFFFFFFC;
    }
    return FALSE;
}

BOOLEAN mergeItem(u8 * oldBuf, u32 oldNVlength, u8 * newBuf, u32 newNVlength)
{
    u32 i;
    u32 oldSize, oldPos;
    u32 newSize, newPos;
    for (i = 0; strcmp(nv_cfg[i].name,"\0"); i++) {
        if (!___findItem(nv_cfg[i].id, oldBuf, oldNVlength, &oldSize, &oldPos)) {
            continue;
        }
        if (!___findItem(nv_cfg[i].id, newBuf, newNVlength, &newSize, &newPos)) {
            continue;
        }
        NVMERGE_TRACE("NVMERGE:__mergeItem oldSize 0x%x newSize 0x%x\n", oldSize, newSize);
        if (oldSize == newSize) {
            memcpy(newBuf + newPos, oldBuf + oldPos, newSize);
            NVMERGE_TRACE("NVMERGE:__mergeItem success id = 0x%x\n", nv_cfg[i].id);
        } else {
            return FALSE;
        }
    }
    return TRUE;
}
