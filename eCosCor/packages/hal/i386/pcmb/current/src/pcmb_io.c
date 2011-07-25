//==========================================================================
//
//      pcmb_io.c
//
//      HAL support code for PCI IDE interface on PC motherboard
//
//==========================================================================
// ####ECOSGPLCOPYRIGHTBEGIN####                                            
// -------------------------------------------                              
// This file is part of eCos, the Embedded Configurable Operating System.   
// Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003 Free Software Foundation, Inc.
//
// eCos is free software; you can redistribute it and/or modify it under    
// the terms of the GNU General Public License as published by the Free     
// Software Foundation; either version 2 or (at your option) any later      
// version.                                                                 
//
// eCos is distributed in the hope that it will be useful, but WITHOUT      
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or    
// FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License    
// for more details.                                                        
//
// You should have received a copy of the GNU General Public License        
// along with eCos; if not, write to the Free Software Foundation, Inc.,    
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.            
//
// As a special exception, if other files instantiate templates or use      
// macros or inline functions from this file, or you compile this file      
// and link it with other works to produce a work based on this file,       
// this file does not by itself cause the resulting work to be covered by   
// the GNU General Public License. However the source code for this file    
// must still be made available in accordance with section (3) of the GNU   
// General Public License v2.                                               
//
// This exception does not invalidate any other reasons why a work based    
// on this file might be covered by the GNU General Public License.         
// -------------------------------------------                              
// ####ECOSGPLCOPYRIGHTEND####                                              
//==========================================================================
//#####DESCRIPTIONBEGIN####
//
// Author(s):    msalter
// Contributors: msalter, gthomas, eak
// Date:         2002-01-04
// Purpose:      PCI support
// Description:  Implementations of HAL PCI IDE interfaces
//
//####DESCRIPTIONEND####
//
//========================================================================*/

#include <pkgconf/system.h>

#include CYGBLD_HAL_PLATFORM_H 

#ifdef CYGHWR_HAL_I386_PC_PCI_IDE

#include <cyg/io/pci.h>
#include <cyg/infra/diag.h>

#define DEBUG

//PCI IDE Controller channels initialized to compatibility mode.
static struct
{
    cyg_uint32 cmd_bar;
    cyg_uint32 ctl_bar;
} ide_ctrl[HAL_IDE_NUM_CONTROLLERS] =
{
	{ 0x1f0, 0x3f4},
	{ 0x170, 0x374}
};

static cyg_bool
find_match_func(cyg_uint16 vendor_id, cyg_uint16 device_id,
                         cyg_uint32 class_id, void *p)
{
  if ((class_id & 0xffff00) == CYG_PCI_CLASS_STORAGE_IDE)
  {
	  *(cyg_uint32 *)p = (class_id) & 0x0f;
	  return true;
  }

  return false;
}

cyg_uint8 cyg_hal_plf_ide_read_uint8(int ctlr, cyg_uint32 reg)
{
	cyg_uint8  val;

	HAL_READ_UINT8(ide_ctrl[ctlr].cmd_bar + reg, val);

	return val;
}

void cyg_hal_plf_ide_write_uint8(int ctlr, cyg_uint32 reg, cyg_uint8 val)
{
	HAL_WRITE_UINT8(ide_ctrl[ctlr].cmd_bar + reg, val);
}

cyg_uint16 cyg_hal_plf_ide_read_uint16(int ctlr, cyg_uint32 reg)
{
	cyg_uint16 val;

	HAL_READ_UINT16(ide_ctrl[ctlr].cmd_bar + reg, val);

	return val;
}

void cyg_hal_plf_ide_write_uint16(int ctlr, cyg_uint32 reg, cyg_uint16 val)
{
	HAL_WRITE_UINT16(ide_ctrl[ctlr].cmd_bar + reg, val);
}

void cyg_hal_plf_ide_write_control(int ctlr, cyg_uint8 val)
{
	HAL_WRITE_UINT8(ide_ctrl[ctlr].ctl_bar + 2, val);
}

int cyg_hal_plf_ide_init(void)
{
    int i;
    cyg_pci_device_id ide_dev = CYG_PCI_NULL_DEVID;
    cyg_pci_device ide_info;
	cyg_uint32 pi;

#ifdef DEBUG
    diag_printf("Initializing PCI IDE controller\n");
#endif

	cyg_pci_init();

//	if (cyg_pci_find_device((cyg_uint16)0x1095, (cyg_uint16)0x0649, &ide_dev))
//	if (cyg_pci_find_class((cyg_uint32)CYG_PCI_CLASS_STORAGE_IDE, &ide_dev))
    if (cyg_pci_find_matching(&find_match_func, &pi,
                               &ide_dev))
        cyg_pci_get_device_info(ide_dev, &ide_info);
	else
	{
#ifdef DEBUG
        diag_printf("Can't find PCI IDE controller!\n");
#endif
        return 0;
    }

	//Configure device.
	if (cyg_pci_configure_device(&ide_info))
	{
		for (i = 0;  i < HAL_IDE_NUM_CONTROLLERS;  i++)
		if (((pi >> (i << 1)) & 0x01) != 0)
		{
            ide_ctrl[i].cmd_bar = ide_info.base_map[(2*i)+0] & 0xFFFFFFFE;
            ide_ctrl[i].ctl_bar = ide_info.base_map[(2*i)+1] & 0xFFFFFFFE;
        }
#ifdef DEBUG
		diag_printf("Found pci ide device on bus %d, devfn 0x%02x:\n",
                CYG_PCI_DEV_GET_BUS(ide_dev),
                CYG_PCI_DEV_GET_DEVFN(ide_dev));

		if (ide_info.command & CYG_PCI_CFG_COMMAND_ACTIVE)
			diag_printf(" The board is active.\n");

		diag_printf(" Vendor    0x%04x", ide_info.vendor);
		diag_printf("\n Device    0x%04x", ide_info.device);
		diag_printf("\n Command   0x%04x, Status 0x%04x\n",
                ide_info.command, ide_info.status);

		diag_printf(" Class/Rev 0x%08x", ide_info.class_rev);
		diag_printf("\n Header 0x%02x\n", ide_info.header_type);

		diag_printf(" SubVendor 0x%04x, Sub ID 0x%04x\n",
                ide_info.header.normal.sub_vendor,
                ide_info.header.normal.sub_id);

		for(i = 0; i < CYG_PCI_MAX_BAR; i++)
		{
			diag_printf(" BAR[%d]    0x%08x /", i, ide_info.base_address[i]);
			diag_printf(" probed size 0x%08x / CPU addr 0x%08x\n",
                  ide_info.base_size[i], ide_info.base_map[i]);
		}
#endif
	}
	else
	{
#ifdef DEBUG
		diag_printf("Failed to configure pci ide device\n");
#endif
		return 0;
	}


	//Activate pci ide device.
	if ((ide_info.command & CYG_PCI_CFG_COMMAND_IO) == 0)
	{
		ide_info.command |= CYG_PCI_CFG_COMMAND_IO;
		cyg_pci_write_config_uint16(ide_info.devid,
			CYG_PCI_CFG_COMMAND, ide_info.command);
	}

    return HAL_IDE_NUM_CONTROLLERS;
}

#endif // CYGHWR_HAL_I386_PC_PCI_IDE
