/*
    DWORD	TabOffset[]
      Brick:
          - BYTE Delta X
          - BYTE Delta Y
        Line(Delta Y):
        - BYTE NbBlock
           Block:
            - 00xxxxxxb  xxxxxx zeros to jump
            - 01xxxxxxb  xxxxxx Copy Pix
            - 10xxxxxxb  xxxxxx Repeat Pix
            - BYTE datas[]
*/

#include <SVGA/CALCMASK.H>
#include <stdio.h>
#include <SYSTEM/DEBUG_TRACES.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/LOGPRINT.H>
#endif

S32 CalcGraphMsk(S32 numbrick, U8 *bankbrick, U8 *ptmask) {
    LIB386_TRACE_CPP("CalcGraphMsk", "1", "num=%d bank=%p mask=%p", numbrick, (void *)bankbrick, (void *)ptmask);
    U8 *srcData = bankbrick + *((U32 *)bankbrick + numbrick);
    U8 *dstData = ptmask;
#ifdef LBA2_TARGET_WIIU
    {
        static int s_logged = 0;
        if (s_logged < 2) {
            U32 raw_off = *((U32 *)bankbrick + numbrick);
            LogPrintf("[CalcGraphMsk] brick %d: raw_offset=0x%08X srcData=%p deltaX=%u deltaY=%u hotX=%u hotY=%u\n",
                      (int)numbrick, (unsigned)raw_off, (void*)srcData,
                      (unsigned)srcData[0], (unsigned)srcData[1],
                      (unsigned)srcData[2], (unsigned)srcData[3]);
            LogPrintf("[CalcGraphMsk] brick %d first 32 bytes: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      (int)numbrick,
                      srcData[0], srcData[1], srcData[2], srcData[3], srcData[4], srcData[5], srcData[6], srcData[7],
                      srcData[8], srcData[9], srcData[10], srcData[11], srcData[12], srcData[13], srcData[14], srcData[15],
                      srcData[16], srcData[17], srcData[18], srcData[19], srcData[20], srcData[21], srcData[22], srcData[23],
                      srcData[24], srcData[25], srcData[26], srcData[27], srcData[28], srcData[29], srcData[30], srcData[31]);
            s_logged++;
        }
    }
#endif
    dstData[0] = srcData[0]; // Delta X
    dstData[1] = srcData[1]; // Delta Y
    dstData[2] = srcData[2]; // Hot X
    dstData[3] = srcData[3]; // Hot Y

    U8 nbLine = srcData[1]; // NbLine (Delta Y)

    srcData += 4;
    dstData += 4;

    do {
        // NextLine:
        // Init NbBlock for this line
        dstData[0] = 0;
        dstData[1] = 0;
        U8 nbBlock = srcData[0];
        U8 *nbBlockDst = dstData;
        srcData++;
        dstData++; // Jump NbBlockDst

        // Flag:
        //   if flag=0   => Code "Jump"
        //   if flag=1   => Code "Mask"
        // We still don't start a "Jump"
        U8 flag = 0;
        do {
            // SameLine:
            U8 opCode = *srcData;
            srcData++;
            U8 count = (opCode & 0x3F) + 1;
            if (opCode & 0x80) {
                // RepeatCol:
                if (!flag) {
                    dstData[1] = 0;
                    dstData++;
                    nbBlockDst[0]++; // Increment Nb of blocks (because block change)
                    flag = 1;
                }
                // @@Same_Block2:
                *dstData += count;
                ++srcData;
            } else if (opCode & 0x40) {
                if (!flag) {
                    dstData[1] = 0;
                    dstData++;
                    nbBlockDst[0]++;
                    flag = 1;
                }
                *dstData += count;
                srcData += count;
            } else {
                if (flag) {
                    ++dstData;
                    flag = 0;
                    nbBlockDst[0]++; // Increment Nb of blocks (because block change)
                    *dstData = 0;
                }
                *dstData += count;
            }
            --nbBlock;
        } while (nbBlock);
        dstData++;
        nbBlockDst[0]++;
        nbLine--;
    } while (nbLine);

    return dstData - ptmask;
}
