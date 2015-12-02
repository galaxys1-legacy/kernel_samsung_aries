<<<<<<< HEAD
/* arch/arm/plat-s5p/include/plat/mfc.h
 *
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 * 		http://www.samsung.com
 *
 * Platform header file for MFC driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef _S3C_MFC_H
#define _S3C_MFC_H

#include <linux/types.h>

struct s3c_platform_mfc {
	dma_addr_t buf_phy_base[2];
	size_t buf_phy_size[2];
};

extern void s3c_mfc_set_platdata(struct s3c_platform_mfc *pd);
#endif
=======
/*
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */

#ifndef __PLAT_S5P_MFC_H
#define __PLAT_S5P_MFC_H

/**
 * s5p_mfc_reserve_mem - function to early reserve memory for MFC driver
 * @rbase:	base address for MFC 'right' memory interface
 * @rsize:	size of the memory reserved for MFC 'right' interface
 * @lbase:	base address for MFC 'left' memory interface
 * @lsize:	size of the memory reserved for MFC 'left' interface
 *
 * This function reserves system memory for both MFC device memory
 * interfaces and registers it to respective struct device entries as
 * coherent memory.
 */
void __init s5p_mfc_reserve_mem(phys_addr_t rbase, unsigned int rsize,
				phys_addr_t lbase, unsigned int lsize);

#endif /* __PLAT_S5P_MFC_H */
>>>>>>> v3.1
