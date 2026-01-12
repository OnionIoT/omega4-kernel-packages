/* SPDX-License-Identifier: GPL-2.0 */
#ifndef RK_VB2_COMPAT_H
#define RK_VB2_COMPAT_H

#include <media/videobuf2-dma-sg.h>

#ifndef vb2_cma_sg_memops
#define vb2_cma_sg_memops vb2_dma_sg_memops
#endif

#endif
