/*------------------------------------------------------------------------/
/  MMCv3/SDv1/SDv2 (in SPI mode) control module
/-------------------------------------------------------------------------/
/
/  Copyright (C) 2012, ChaN, all right reserved.
/
/ * This software is a free software and there is NO WARRANTY.
/ * No restriction on use. You can use, modify and redistribute it for
/   personal, non-profit or commercial products UNDER YOUR RESPONSIBILITY.
/ * Redistributions of source code must retain the above copyright notice.
/
/ *  Author               Date        Comment
/ *~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
/ * Govind Mukundan      02/15/2012    Modified to integrate with Microchip USB stack (Host mode)
/
/-------------------------------------------------------------------------*/


#include <p24FJ256GB106.h>
#include "HardwareProfile.h"
#include "diskio.h"
#include "MDD\SD-SPI.h"
#include "USB/usb_host_msd_scsi.h"


extern void OpenSPIM(unsigned int sync_mode);
/* Definitions for MMC/SDC command */
#define CMD0   (0)			/* GO_IDLE_STATE */
#define CMD1   (1)			/* SEND_OP_COND */
#define ACMD41 (41|0x80)	/* SEND_OP_COND (SDC) */
#define CMD8   (8)			/* SEND_IF_COND */
#define CMD9   (9)			/* SEND_CSD */
#define CMD10  (10)			/* SEND_CID */
#define CMD12  (12)			/* STOP_TRANSMISSION */
#define ACMD13 (13|0x80)	/* SD_STATUS (SDC) */
#define CMD16  (16)			/* SET_BLOCKLEN */
#define CMD17  (17)			/* READ_SINGLE_BLOCK */
#define CMD18  (18)			/* READ_MULTIPLE_BLOCK */
#define CMD23  (23)			/* SET_BLOCK_COUNT */
#define ACMD23 (23|0x80)	/* SET_WR_BLK_ERASE_COUNT (SDC) */
#define CMD24  (24)			/* WRITE_BLOCK */
#define CMD25  (25)			/* WRITE_MULTIPLE_BLOCK */
#define CMD41  (41)			/* SEND_OP_COND (ACMD) */
#define CMD55  (55)			/* APP_CMD */
#define CMD58  (58)			/* READ_OCR */


/* Port Controls  (Platform dependent) */
#define CS_LOW()  SD_CS = 0	/* MMC CS = L */
#define CS_HIGH() SD_CS = 1	/* MMC CS = H */

#define SOCKWP	(0)	/* Write protected (yes:true, no:false, default:false) */
#define SOCKINS	(1)	/* Card inserted   (yes:true, no:false, default:true) */

#define	FCLK_SLOW()	OpenSPIM(MASTER_ENABLE_ON | PRI_PRESCAL_1_1 | SEC_PRESCAL_1_1);		/* Set slow clock (100k-400k) */
// with 16MHz Fcy, 0x3E -> 4000k Hz, 0x3B -> 8000k Hz
#define	FCLK_FAST()	OpenSPIM(0x3B);		/* Set fast clock (depends on the CSD) */ //0x3E



/*--------------------------------------------------------------------------

   Module Private Functions

---------------------------------------------------------------------------*/

static volatile
DSTATUS Stat = STA_NOINIT; /* Disk status */

static volatile
UINT Timer1, Timer2; /* 1000Hz decrement timer */

static
UINT CardType;



/*-----------------------------------------------------------------------*/
/* Exchange a byte between PIC and MMC via SPI  (Platform dependent)     */
/*-----------------------------------------------------------------------*/

#define xmit_spi(dat) 	xchg_spi(dat)
#define rcvr_spi()	xchg_spi(0xFF)
#define rcvr_spi_m(p)	SPI1BUF = 0xFF; while (!SPISTAT_RBF); *(p) = (BYTE)SPI1BUF;

static
BYTE xchg_spi(BYTE dat)
{
    SPI1BUF = dat;
    while (!SPISTAT_RBF);
    return (BYTE) SPI1BUF;
}


/*-----------------------------------------------------------------------*/
/* Wait for card ready                                                   */

/*-----------------------------------------------------------------------*/

static
int wait_ready(void)
{
    BYTE d;

    Timer2 = 500; /* Wait for ready in timeout of 500ms */
    do
    {
        d = rcvr_spi();
    }
    while ((d != 0xFF) && Timer2);

    return (d == 0xFF) ? 1 : 0;
}



/*-----------------------------------------------------------------------*/
/* Deselect the card and release SPI bus                                 */

/*-----------------------------------------------------------------------*/

static
void deselect(void)
{
    CS_HIGH();
    rcvr_spi(); /* Dummy clock (force DO hi-z for multiple slave SPI) */
}



/*-----------------------------------------------------------------------*/
/* Select the card and wait ready                                        */

/*-----------------------------------------------------------------------*/

static
int select(void) /* 1:Successful, 0:Timeout */
{
    CS_LOW();
    rcvr_spi(); /* Dummy clock (force DO enabled) */

    if (wait_ready()) return 1; /* OK */
    deselect();
    return 0; /* Timeout */
}



/*-----------------------------------------------------------------------*/
/* Power Control  (Platform dependent)                                   */
/*-----------------------------------------------------------------------*/
/* When the target system does not support socket power control, there   */

/* is nothing to do in these functions.                                  */

static
void power_on(void)
{
    ; /* Turn on socket power, delay >1ms */

    //SPI1CON1 = 0x013B;	/* Enable SPI1 */
    //SPI1CON2 = 0x0000;
    //SPIENABLE = 1;

    SPICON1 = 0x0000; // power on state
    SPICON1 |= MASTER_ENABLE_ON | 0x1C | 0x00; // select serial mode
    SPICON1bits.CKP = 1;
    SPICON1bits.CKE = 0;

    SPICLOCK = 0;
    SPIOUT = 0; // define SDO1 as output (master or slave)
    SPIIN = 1; // define SDI1 as input (master or slave)
    SPIENABLE = 1; // enable synchronous serial port
}

static
void power_off(void)
{
    select(); /* Wait for card ready */
    deselect();

    SPIENABLE = 0; /* Disable SPI1 */

    ; /* Turn off socket power */

    Stat |= STA_NOINIT; /* Force uninitialized */
}



/*-----------------------------------------------------------------------*/
/* Receive a data packet from MMC                                        */

/*-----------------------------------------------------------------------*/

static
int rcvr_datablock(/* 1:OK, 0:Failed */
                   BYTE *buff, /* Data buffer to store received data */
                   UINT btr /* Byte count (must be multiple of 4) */
                   )
{
    BYTE token;


    Timer1 = 100;
    do
    { /* Wait for data packet in timeout of 100ms */
        token = rcvr_spi();
    }
    while ((token == 0xFF) && Timer1);

    if (token != 0xFE) return 0; /* If not valid data token, retutn with error */

    do
    { /* Receive the data block into buffer */
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
        rcvr_spi_m(buff++);
    }
    while (btr -= 4);
    rcvr_spi(); /* Discard CRC */
    rcvr_spi();

    return 1; /* Return with success */
}



/*-----------------------------------------------------------------------*/
/* Send a data packet to MMC                                             */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0

static
int xmit_datablock(/* 1:OK, 0:Failed */
                   const BYTE *buff, /* 512 byte data block to be transmitted */
                   BYTE token /* Data token */
                   )
{
    BYTE resp;
    UINT bc = 512;


    if (!wait_ready()) return 0;

    xmit_spi(token); /* Xmit a token */
    if (token != 0xFD)
    { /* Not StopTran token */
        do
        { /* Xmit the 512 byte data block to the MMC */
            xmit_spi(*buff++);
            xmit_spi(*buff++);
        }
        while (bc -= 2);
        xmit_spi(0xFF); /* CRC (Dummy) */
        xmit_spi(0xFF);
        resp = rcvr_spi(); /* Receive a data response */
        if ((resp & 0x1F) != 0x05) /* If not accepted, return with error */
            return 0;
    }

    return 1;
}
#endif	/* _READONLY */



/*-----------------------------------------------------------------------*/
/* Send a command packet to MMC                                          */

/*-----------------------------------------------------------------------*/

static
BYTE send_cmd(
              BYTE cmd, /* Command byte */
              DWORD arg /* Argument */
              )
{
    BYTE n, res;


    if (cmd & 0x80)
    { /* ACMD<n> is the command sequense of CMD55-CMD<n> */
        cmd &= 0x7F;
        res = send_cmd(CMD55, 0);
        if (res > 1) return res;
    }

    /* Select the card and wait for ready */
    deselect();
    if (!select()) return 0xFF;

    /* Send command packet */
    xmit_spi(0x40 | cmd); /* Start + Command index */
    xmit_spi((BYTE) (arg >> 24)); /* Argument[31..24] */
    xmit_spi((BYTE) (arg >> 16)); /* Argument[23..16] */
    xmit_spi((BYTE) (arg >> 8)); /* Argument[15..8] */
    xmit_spi((BYTE) arg); /* Argument[7..0] */
    n = 0x01; /* Dummy CRC + Stop */
    if (cmd == CMD0) n = 0x95; /* Valid CRC for CMD0(0) */
    if (cmd == CMD8) n = 0x87; /* Valid CRC for CMD8(0x1AA) */
    xmit_spi(n);

    /* Receive command response */
    if (cmd == CMD12) rcvr_spi(); /* Skip a stuff byte when stop reading */
    n = 10; /* Wait for a valid response in timeout of 10 attempts */
    do
        res = rcvr_spi();
    while ((res & 0x80) && --n);

    return res; /* Return with the response value */
}



/*--------------------------------------------------------------------------

   Public Functions

---------------------------------------------------------------------------*/

/*-----------------------------------------------------------------------*/
/* Initialize Disk Drive                                                 */

/*-----------------------------------------------------------------------*/


DSTATUS disk_initialize(
                        BYTE drv /* Physical drive nmuber (0) */
                        )
{
    BYTE n, cmd, ty, ocr[4];


    switch (drv)
    {
    case C_DRIVE_SDCARD:
        if (Stat & STA_NODISK) return Stat; /* No card in the socket */

        power_on(); /* Force socket power on */
        //FCLK_SLOW();
        for (n = 10; n; n--) rcvr_spi(); /* 80 dummy clocks */

        ty = 0;
        if (send_cmd(CMD0, 0) == 1)
        { /* Enter Idle state */
            Timer1 = 1000; /* Initialization timeout of 1000 msec */
            if (send_cmd(CMD8, 0x1AA) == 1)
            { /* SDv2? */
                for (n = 0; n < 4; n++) ocr[n] = rcvr_spi(); /* Get trailing return value of R7 resp */
                if (ocr[2] == 0x01 && ocr[3] == 0xAA)
                { /* The card can work at vdd range of 2.7-3.6V */
                    while (Timer1 && send_cmd(ACMD41, 0x40000000)); /* Wait for leaving idle state (ACMD41 with HCS bit) */
                    if (Timer1 && send_cmd(CMD58, 0) == 0)
                    { /* Check CCS bit in the OCR */
                        for (n = 0; n < 4; n++) ocr[n] = rcvr_spi();
                        ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2; /* SDv2 */
                    }
                }
            }
            else
            { /* SDv1 or MMCv3 */
                if (send_cmd(ACMD41, 0) <= 1)
                {
                    ty = CT_SD1;
                    cmd = ACMD41; /* SDv1 */
                }
                else
                {
                    ty = CT_MMC;
                    cmd = CMD1; /* MMCv3 */
                }
                while (Timer1 && send_cmd(cmd, 0)); /* Wait for leaving idle state */
                if (!Timer1 || send_cmd(CMD16, 512) != 0) /* Set read/write block length to 512 */
                    ty = 0;
            }
        }
        CardType = ty;
        deselect();

        if (ty)
        { /* Initialization succeded */
            Stat &= ~STA_NOINIT; /* Clear STA_NOINIT */
            FCLK_FAST();
        }
        else
        { /* Initialization failed */
            power_off();
            addToTerminalLog("Media Init Failed");
        }

        return Stat;

        break;


    case C_DRIVE_USB:
        if (!USBHostMSDSCSIMediaInitialize())
        {
            return STA_NODISK;
        }
        return 0;
        break;


    default:
        return STA_NOINIT;
        break;

    }
}



/*-----------------------------------------------------------------------*/
/* Get Disk Status                                                       */

/*-----------------------------------------------------------------------*/

DSTATUS disk_status(
                    BYTE drv /* Physical drive nmuber (0) */
                    )
{

    switch (drv)
    {
    case C_DRIVE_SDCARD:
        return Stat;

        break;


    case C_DRIVE_USB:
        if (!USBHostMSDSCSIMediaDetect())
        {
            return STA_NODISK;
        }
        return 0;

        break;


    default:
        return STA_NOINIT; /* Unsupported drive */
        break;

    }


}



/*-----------------------------------------------------------------------*/
/* Read Sector(s)                                                        */

/*-----------------------------------------------------------------------*/

DRESULT disk_read(
                  BYTE drv, /* Physical drive nmuber (0) */
                  BYTE *buff, /* Pointer to the data buffer to store read data */
                  DWORD sector, /* Start sector number (LBA) */
                  BYTE count /* Sector count (1..255) */
                  )
{
        int nSector;
    switch (drv)
    {
    case C_DRIVE_SDCARD:
        if (!count) return RES_PARERR;
        if (Stat & STA_NOINIT) return RES_NOTRDY;

        if (!(CardType & CT_BLOCK)) sector *= 512; /* Convert to byte address if needed */

        if (count == 1)
        { /* Single block read */
            if ((send_cmd(CMD17, sector) == 0) /* READ_SINGLE_BLOCK */
                    && rcvr_datablock(buff, 512))
                count = 0;
        }
        else
        { /* Multiple block read */
            if (send_cmd(CMD18, sector) == 0)
            { /* READ_MULTIPLE_BLOCK */
                do
                {
                    if (!rcvr_datablock(buff, 512)) break;
                    buff += 512;
                }
                while (--count);
                send_cmd(CMD12, 0); /* STOP_TRANSMISSION */
            }
        }
        deselect();

        return count ? RES_ERROR : RES_OK;

        break;


    case C_DRIVE_USB:

        for (nSector = 0; nSector < count; nSector++)
        {
            if (!USBHostMSDSCSISectorRead(sector, buff))
            {
                return RES_NOTRDY;
            }
            sector++;
            buff += 512;
        }
        return RES_OK;

        break;


    default:
        return RES_PARERR; /* Unsupported drive */
        break;

    }



}



/*-----------------------------------------------------------------------*/
/* Write Sector(s)                                                       */
/*-----------------------------------------------------------------------*/

#if _READONLY == 0

DRESULT disk_write(
                   BYTE drv, /* Physical drive nmuber (0) */
                   const BYTE *buff, /* Pointer to the data to be written */
                   DWORD sector, /* Start sector number (LBA) */
                   BYTE count /* Sector count (1..255) */
                   )
{
        int nSector;
    switch (drv)
    {
    case C_DRIVE_SDCARD:
        if (!count) return RES_PARERR;
        if (Stat & STA_NOINIT) return RES_NOTRDY;
        if (Stat & STA_PROTECT) return RES_WRPRT;

        if (!(CardType & CT_BLOCK)) sector *= 512; /* Convert to byte address if needed */

        if (count == 1)
        { /* Single block write */
            if ((send_cmd(CMD24, sector) == 0) /* WRITE_BLOCK */
                    && xmit_datablock(buff, 0xFE))
                count = 0;
        }
        else
        { /* Multiple block write */
            if (CardType & CT_SDC) send_cmd(ACMD23, count);
            if (send_cmd(CMD25, sector) == 0)
            { /* WRITE_MULTIPLE_BLOCK */
                do
                {
                    if (!xmit_datablock(buff, 0xFC)) break;
                    buff += 512;
                }
                while (--count);
                if (!xmit_datablock(0, 0xFD)) /* STOP_TRAN token */
                    count = 1;
            }
        }
        deselect();

        return count ? RES_ERROR : RES_OK;

        break;


    case C_DRIVE_USB:
    for (nSector = 0; nSector < count; nSector++)
        {
            if (!USBHostMSDSCSISectorWrite(sector, (BYTE *) buff, 0))
            {
                return RES_NOTRDY;
            }
            sector++;
            buff += 512;
        }
        return RES_OK;

        break;


    default:
        return RES_PARERR; /* Unsupported drive */
        break;

    }



}
#endif /* _READONLY */



/*-----------------------------------------------------------------------*/
/* Miscellaneous Functions                                               */

/*-----------------------------------------------------------------------*/

DRESULT disk_ioctl(
                   BYTE drv, /* Physical drive nmuber (0) */
                   BYTE ctrl, /* Control code */
                   void *buff /* Buffer to send/receive data block */
                   )
{
        DRESULT res;
        BYTE n, csd[16], *ptr = buff;
        DWORD csize;
    switch (drv)
    {
    case C_DRIVE_SDCARD:
        if (drv) return RES_PARERR;
        if (Stat & STA_NOINIT) return RES_NOTRDY;

        res = RES_ERROR;
        switch (ctrl)
        {
        case CTRL_SYNC: /* Flush write-back cache, Wait for end of internal process */
            if (select())
            {
                deselect();
                res = RES_OK;
            }
            break;

        case GET_SECTOR_COUNT: /* Get number of sectors on the disk (WORD) */
            if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16))
            {
                if ((csd[0] >> 6) == 1)
                { /* SDv2? */
                    csize = csd[9] + ((WORD) csd[8] << 8) + 1;
                    *(DWORD*) buff = (DWORD) csize << 10;
                }
                else
                { /* SDv1 or MMCv3 */
                    n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                    csize = (csd[8] >> 6) + ((WORD) csd[7] << 2) + ((WORD) (csd[6] & 3) << 10) + 1;
                    *(DWORD*) buff = (DWORD) csize << (n - 9);
                }
                res = RES_OK;
            }
            break;

        case GET_SECTOR_SIZE: /* Get sectors on the disk (WORD) */
            *(WORD*) buff = 512;
            res = RES_OK;
            break;

        case GET_BLOCK_SIZE: /* Get erase block size in unit of sectors (DWORD) */
            if (CardType & CT_SD2)
            { /* SDv2? */
                if (send_cmd(ACMD13, 0) == 0)
                { /* Read SD status */
                    rcvr_spi();
                    if (rcvr_datablock(csd, 16))
                    { /* Read partial block */
                        for (n = 64 - 16; n; n--) rcvr_spi(); /* Purge trailing data */
                        *(DWORD*) buff = 16UL << (csd[10] >> 4);
                        res = RES_OK;
                    }
                }
            }
            else
            { /* SDv1 or MMCv3 */
                if ((send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16))
                { /* Read CSD */
                    if (CardType & CT_SD1)
                    { /* SDv1 */
                        *(DWORD*) buff = (((csd[10] & 63) << 1) + ((WORD) (csd[11] & 128) >> 7) + 1) << ((csd[13] >> 6) - 1);
                    }
                    else
                    { /* MMCv3 */
                        *(DWORD*) buff = ((WORD) ((csd[10] & 124) >> 2) + 1) * (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
                    }
                    res = RES_OK;
                }
            }
            break;

        case MMC_GET_TYPE: /* Get card type flags (1 byte) */
            *ptr = CardType;
            res = RES_OK;
            break;

        case MMC_GET_CSD: /* Receive CSD as a data block (16 bytes) */
            if ((send_cmd(CMD9, 0) == 0) /* READ_CSD */
                    && rcvr_datablock(buff, 16))
                res = RES_OK;
            break;

        case MMC_GET_CID: /* Receive CID as a data block (16 bytes) */
            if ((send_cmd(CMD10, 0) == 0) /* READ_CID */
                    && rcvr_datablock(buff, 16))
                res = RES_OK;
            break;

        case MMC_GET_OCR: /* Receive OCR as an R3 resp (4 bytes) */
            if (send_cmd(CMD58, 0) == 0)
            { /* READ_OCR */
                for (n = 0; n < 4; n++)
                    *((BYTE*) buff + n) = rcvr_spi();
                res = RES_OK;
            }
            break;

        case MMC_GET_SDSTAT: /* Receive SD statsu as a data block (64 bytes) */
            if ((CardType & CT_SD2) && send_cmd(ACMD13, 0) == 0)
            { /* SD_STATUS */
                rcvr_spi();
                if (rcvr_datablock(buff, 64))
                    res = RES_OK;
            }
            break;

        default:
            res = RES_PARERR;
        }

        deselect();

        return res;

        break;


    case C_DRIVE_USB:
        switch (ctrl)
        {
        case GET_SECTOR_SIZE:
            *(WORD *) buff = 512;
            return RES_OK;

#if _READONLY == 0
        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_COUNT:
            *(DWORD *) buff = 0; // Number of sectors on the volume
            return RES_OK;

        case GET_BLOCK_SIZE:
            return RES_OK;
#endif
        }
        return RES_PARERR;

        break;


    default:
        return RES_PARERR; /* Unsupported drive */
        break;

    }

}



/*-----------------------------------------------------------------------*/
/* Device Timer Interrupt Procedure                                      */
/*-----------------------------------------------------------------------*/

/* This function must be called by timer interrupt in period of 1ms      */

void disk_timerproc(void)
{
    BYTE s;
    UINT n;


    n = Timer1; /* 1000Hz decrement timer with zero stopped */
    if (n) Timer1 = --n;
    n = Timer2;
    if (n) Timer2 = --n;


    /* Update socket status */

    s = Stat;

    if (SOCKWP) s |= STA_PROTECT;
    else s &= ~STA_PROTECT;

    if (SOCKINS) s &= ~STA_NODISK;
    else s |= (STA_NODISK | STA_NOINIT);

    Stat = s;
}

