/* 
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (c) 2013 China Beijing Armink <armink.ztl@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * File: $Id: mbrtu_m.c,v 1.60 2013/08/17 11:42:56 Armink Add Master Functions $
 */

/* ----------------------- System includes ----------------------------------*/
#include "stdlib.h"
#include "string.h"

/* ----------------------- Platform includes --------------------------------*/
#include "port_2.h"

/* ----------------------- Modbus includes ----------------------------------*/
#include "mb_2.h"
#include "mb_m_2.h"
#include "mbrtu_2.h"
#include "mbframe_2.h"

#include "mbcrc_2.h"
#include "mbport_2.h"

#if MB_MASTER_RTU_ENABLED > 0
/* ----------------------- Defines ------------------------------------------*/
#define MB_SER_PDU_SIZE_MIN     4       /*!< Minimum size of a Modbus RTU frame. */
#define MB_SER_PDU_SIZE_MAX     256     /*!< Maximum size of a Modbus RTU frame. */
#define MB_SER_PDU_SIZE_CRC     2       /*!< Size of CRC field in PDU. */
#define MB_SER_PDU_ADDR_OFF     0       /*!< Offset of slave address in Ser-PDU. */
#define MB_SER_PDU_PDU_OFF      1       /*!< Offset of Modbus-PDU in Ser-PDU. */

/* ----------------------- Type definitions ---------------------------------*/
typedef enum
{
    STATE_M_RX_INIT,              /*!< Receiver is in initial state. */
    STATE_M_RX_IDLE,              /*!< Receiver is in idle state. */
    STATE_M_RX_RCV,               /*!< Frame is beeing received. */
    STATE_M_RX_ERROR,              /*!< If the frame is invalid. */
} eMBMasterRcvState;

typedef enum
{
    STATE_M_TX_IDLE,              /*!< Transmitter is in idle state. */
    STATE_M_TX_XMIT,              /*!< Transmitter is in transfer state. */
    STATE_M_TX_XFWR,              /*!< Transmitter is in transfer finish and wait receive state. */
} eMBMasterSndState;

/* ----------------------- Static variables ---------------------------------*/
static volatile eMBMasterSndState eSndState;
static volatile eMBMasterRcvState eRcvState;

 volatile UCHAR  ucMasterRTUSndBuf_2[MB_PDU_SIZE_MAX];
 volatile UCHAR  ucMasterRTURcvBuf_2[MB_SER_PDU_SIZE_MAX];
static volatile USHORT usMasterSendPDULength;

static volatile UCHAR *pucMasterSndBufferCur;
static volatile USHORT usMasterSndBufferCount;

static volatile USHORT usMasterRcvBufferPos;
static volatile BOOL   xFrameIsBroadcast = FALSE;

static volatile eMBMasterTimerMode eMasterCurTimerMode;

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode
eMBMasterRTUInit_2(UCHAR ucPort, ULONG ulBaudRate, eMBParity eParity )
{
    eMBErrorCode    eStatus = MB_ENOERR;
    ULONG           usTimerT35_50us;

    ENTER_CRITICAL_SECTION(  );

    /* Modbus RTU uses 8 Databits. */
    if( xMBMasterPortSerialInit_2( ucPort, ulBaudRate, 8, eParity ) != TRUE )
    {
        eStatus = MB_EPORTERR;
    }
    else
    {
        /* If baudrate > 19200 then we should use the fixed timer values
         * t35 = 1750us. Otherwise t35 must be 3.5 times the character time.
         */
        if( ulBaudRate > 19200 )
        {
            usTimerT35_50us = 35;       /* 1800us. */
        }
        else
        {
            /* The timer reload value for a character is given by:
             *
             * ChTimeValue = Ticks_per_1s / ( Baudrate / 11 )
             *             = 11 * Ticks_per_1s / Baudrate
             *             = 220000 / Baudrate
             * The reload for t3.5 is 1.5 times this value and similary
             * for t3.5.
             */
            usTimerT35_50us = ( 7UL * 220000UL ) / ( 2UL * ulBaudRate );
        }
        if( xMBMasterPortTimersInit_2( ( USHORT ) usTimerT35_50us ) != TRUE )
        {
            eStatus = MB_EPORTERR;
        }
    }
    EXIT_CRITICAL_SECTION(  );

    return eStatus;
}

void
eMBMasterRTUStart_2( void )
{
    ENTER_CRITICAL_SECTION(  );
    /* Initially the receiver is in the state STATE_M_RX_INIT. we start
     * the timer and if no character is received within t3.5 we change
     * to STATE_M_RX_IDLE. This makes sure that we delay startup of the
     * modbus protocol stack until the bus is free.
     */
    eRcvState = STATE_M_RX_INIT;
    vMBMasterPortSerialEnable_2( TRUE, FALSE );
    vMBMasterPortTimersT35Enable_2(  );

    EXIT_CRITICAL_SECTION(  );
}

void
eMBMasterRTUStop_2( void )
{
    ENTER_CRITICAL_SECTION(  );
    vMBMasterPortSerialEnable_2( FALSE, FALSE );
    vMBMasterPortTimersDisable_2(  );
    EXIT_CRITICAL_SECTION(  );
}

eMBErrorCode
eMBMasterRTUReceive_2( UCHAR * pucRcvAddress, UCHAR ** pucFrame, USHORT * pusLength )
{
    eMBErrorCode    eStatus = MB_ENOERR;

    ENTER_CRITICAL_SECTION(  );
    assert_param( usMasterRcvBufferPos < MB_SER_PDU_SIZE_MAX );

    /* Length and CRC check */
    if( ( usMasterRcvBufferPos >= MB_SER_PDU_SIZE_MIN )
        && ( usMBCRC16( ( UCHAR * ) ucMasterRTURcvBuf_2, usMasterRcvBufferPos ) == 0 ) )
    {
        /* Save the address field. All frames are passed to the upper layed
         * and the decision if a frame is used is done there.
         */
        *pucRcvAddress = ucMasterRTURcvBuf_2[MB_SER_PDU_ADDR_OFF];

        /* Total length of Modbus-PDU is Modbus-Serial-Line-PDU minus
         * size of address field and CRC checksum.
         */
        *pusLength = ( USHORT )( usMasterRcvBufferPos - MB_SER_PDU_PDU_OFF - MB_SER_PDU_SIZE_CRC );

        /* Return the start of the Modbus PDU to the caller. */
        *pucFrame = ( UCHAR * ) & ucMasterRTURcvBuf_2[MB_SER_PDU_PDU_OFF];
    }
    else
    {
        eStatus = MB_EIO;
    }

    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}



eMBErrorCode
eMBMaster_Send_not_datas_2(UCHAR * pucFrame, USHORT usLength )//发送非标准的数据
{
    eMBErrorCode    eStatus = MB_ENOERR;
    USHORT          usCRC16;


    ENTER_CRITICAL_SECTION(  );

    /* Check if the receiver is still in idle state. If not we where to
     * slow with processing the received frame and the master sent another
     * frame on the network. We have to abort sending the frame.
     */
    if( eRcvState == STATE_M_RX_IDLE )
    {
        /* First byte before the Modbus-PDU is the slave address. */
        pucMasterSndBufferCur = ( UCHAR * ) pucFrame;
        usMasterSndBufferCount = usLength;


        /* Activate the transmitter. */
        eSndState = STATE_M_TX_XMIT;
        vMBMasterPortSerialEnable_2( FALSE, TRUE );
    }
    else
    {
        eStatus = MB_EIO;
    }
    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}

extern u8 mb_switch_flag_2;
extern u8 mb_not_rtu_data_len_2;
extern u8 rs485_send_buf_not_modbus_2[50];
u8 rs485_send_buf_not_modbus_2[50];

eMBErrorCode
eMBMasterRTUSend_2( UCHAR ucSlaveAddress, const UCHAR * pucFrame, USHORT usLength )
{
    eMBErrorCode    eStatus = MB_ENOERR;
    USHORT          usCRC16;

    if(mb_switch_flag_2)
    {
        return (eMBMaster_Send_not_datas_2(rs485_send_buf_not_modbus_2,mb_not_rtu_data_len_2) );
        
    }
    
    if ( ucSlaveAddress > MB_MASTER_TOTAL_SLAVE_NUM ) return MB_EINVAL;

    ENTER_CRITICAL_SECTION(  );

    /* Check if the receiver is still in idle state. If not we where to
     * slow with processing the received frame and the master sent another
     * frame on the network. We have to abort sending the frame.
     */
    if( eRcvState == STATE_M_RX_IDLE )
    {
        /* First byte before the Modbus-PDU is the slave address. */
        pucMasterSndBufferCur = ( UCHAR * ) pucFrame - 1;
        usMasterSndBufferCount = 1;

        /* Now copy the Modbus-PDU into the Modbus-Serial-Line-PDU. */
        pucMasterSndBufferCur[MB_SER_PDU_ADDR_OFF] = ucSlaveAddress;
        usMasterSndBufferCount += usLength;

        /* Calculate CRC16 checksum for Modbus-Serial-Line-PDU. */
        usCRC16 = usMBCRC16( ( UCHAR * ) pucMasterSndBufferCur, usMasterSndBufferCount );
        ucMasterRTUSndBuf_2[usMasterSndBufferCount++] = ( UCHAR )( usCRC16 & 0xFF );
        ucMasterRTUSndBuf_2[usMasterSndBufferCount++] = ( UCHAR )( usCRC16 >> 8 );

        /* Activate the transmitter. */
        eSndState = STATE_M_TX_XMIT;
        vMBMasterPortSerialEnable_2( FALSE, TRUE );
    }
    else
    {
        eStatus = MB_EIO;
    }
    EXIT_CRITICAL_SECTION(  );
    return eStatus;
}




BOOL
xMBMasterRTUReceiveFSM_2( void )
{
    BOOL            xTaskNeedSwitch = FALSE;
    UCHAR           ucByte;

    assert_param(( eSndState == STATE_M_TX_IDLE ) || ( eSndState == STATE_M_TX_XFWR ));

    /* Always read the character. */
    ( void )xMBMasterPortSerialGetByte_2( ( CHAR * ) & ucByte );

    switch ( eRcvState )
    {
        /* If we have received a character in the init state we have to
         * wait until the frame is finished.
         */
    case STATE_M_RX_INIT:
        vMBMasterPortTimersT35Enable_2( );
        break;

        /* In the error state we wait until all characters in the
         * damaged frame are transmitted.
         */
    case STATE_M_RX_ERROR:
        vMBMasterPortTimersT35Enable_2( );
        break;

        /* In the idle state we wait for a new character. If a character
         * is received the t1.5 and t3.5 timers are started and the
         * receiver is in the state STATE_RX_RECEIVCE and disable early
         * the timer of respond timeout .
         */
    case STATE_M_RX_IDLE:
    	/* In time of respond timeout,the receiver receive a frame.
    	 * Disable timer of respond timeout and change the transmiter state to idle.
    	 */
    	vMBMasterPortTimersDisable_2( );
    	eSndState = STATE_M_TX_IDLE;

        usMasterRcvBufferPos = 0;
        ucMasterRTURcvBuf_2[usMasterRcvBufferPos++] = ucByte;
        eRcvState = STATE_M_RX_RCV;

        /* Enable t3.5 timers. */
        vMBMasterPortTimersT35Enable_2( );
        break;

        /* We are currently receiving a frame. Reset the timer after
         * every character received. If more than the maximum possible
         * number of bytes in a modbus frame is received the frame is
         * ignored.
         */
    case STATE_M_RX_RCV:
        if( usMasterRcvBufferPos < MB_SER_PDU_SIZE_MAX )
        {
            ucMasterRTURcvBuf_2[usMasterRcvBufferPos++] = ucByte;
        }
        else
        {
            eRcvState = STATE_M_RX_ERROR;
        }
        vMBMasterPortTimersT35Enable_2();
        break;
    }
    return xTaskNeedSwitch;
}

BOOL
xMBMasterRTUTransmitFSM_2( void )
{
    BOOL            xNeedPoll = FALSE;

    assert_param( eRcvState == STATE_M_RX_IDLE );

    switch ( eSndState )
    {
        /* We should not get a transmitter event if the transmitter is in
         * idle state.  */
    case STATE_M_TX_IDLE:
        /* enable receiver/disable transmitter. */
        vMBMasterPortSerialEnable_2( TRUE, FALSE );
        break;

    case STATE_M_TX_XMIT:
        /* check if we are finished. */
        if( usMasterSndBufferCount != 0 )
        {
            xMBMasterPortSerialPutByte_2( ( CHAR )*pucMasterSndBufferCur );
            pucMasterSndBufferCur++;  /* next byte in sendbuffer. */
            usMasterSndBufferCount--;
        }
        else
        {
            xFrameIsBroadcast = ( ucMasterRTUSndBuf_2[MB_SER_PDU_ADDR_OFF] == MB_ADDRESS_BROADCAST ) ? TRUE : FALSE;
            /* Disable transmitter. This prevents another transmit buffer
             * empty interrupt. */
            vMBMasterPortSerialEnable_2( TRUE, FALSE );
            eSndState = STATE_M_TX_XFWR;
            /* If the frame is broadcast ,master will enable timer of convert delay,
             * else master will enable timer of respond timeout. */
            if ( xFrameIsBroadcast == TRUE )
            {
            	vMBMasterPortTimersConvertDelayEnable_2( );
            }
            else
            {
            	vMBMasterPortTimersRespondTimeoutEnable_2( );
            }
        }
        break;
    }

    return xNeedPoll;
}

BOOL
xMBMasterRTUTimerExpired_2(void)
{
	BOOL xNeedPoll = FALSE;

	switch (eRcvState)
	{
		/* Timer t35 expired. Startup phase is finished. */
	case STATE_M_RX_INIT:
		xNeedPoll = xMBMasterPortEventPost_2(EV_MASTER_READY);
		break;

		/* A frame was received and t35 expired. Notify the listener that
		 * a new frame was received. */
	case STATE_M_RX_RCV:
		xNeedPoll = xMBMasterPortEventPost_2(EV_MASTER_FRAME_RECEIVED);
		break;

		/* An error occured while receiving the frame. */
	case STATE_M_RX_ERROR:
		vMBMasterSetErrorType_2(EV_ERROR_RECEIVE_DATA);
		xNeedPoll = xMBMasterPortEventPost_2( EV_MASTER_ERROR_PROCESS );
		break;

		/* Function called in an illegal state. */
	default:
		assert_param(
				( eRcvState == STATE_M_RX_INIT ) || ( eRcvState == STATE_M_RX_RCV ) ||
				( eRcvState == STATE_M_RX_ERROR ) || ( eRcvState == STATE_M_RX_IDLE ));
		break;
	}
	eRcvState = STATE_M_RX_IDLE;

	switch (eSndState)
	{
		/* A frame was send finish and convert delay or respond timeout expired.
		 * If the frame is broadcast,The master will idle,and if the frame is not
		 * broadcast.Notify the listener process error.*/
	case STATE_M_TX_XFWR:
		if ( xFrameIsBroadcast == FALSE ) {
			vMBMasterSetErrorType_2(EV_ERROR_RESPOND_TIMEOUT);
			xNeedPoll = xMBMasterPortEventPost_2(EV_MASTER_ERROR_PROCESS);
		}
		break;
		/* Function called in an illegal state. */
	default:
		assert_param(
				( eSndState == STATE_M_TX_XFWR ) || ( eSndState == STATE_M_TX_IDLE ));
		break;
	}
	eSndState = STATE_M_TX_IDLE;

	vMBMasterPortTimersDisable_2( );
	/* If timer mode is convert delay, the master event then turns EV_MASTER_EXECUTE status. */
	if (eMasterCurTimerMode == MB_TMODE_CONVERT_DELAY) {
		xNeedPoll = xMBMasterPortEventPost_2( EV_MASTER_EXECUTE );
	}

	return xNeedPoll;
}

/* Get Modbus Master send RTU's buffer address pointer.*/
void vMBMasterGetRTUSndBuf_2( UCHAR ** pucFrame )
{
	*pucFrame = ( UCHAR * ) ucMasterRTUSndBuf_2;
}

/* Get Modbus Master send PDU's buffer address pointer.*/
void vMBMasterGetPDUSndBuf_2( UCHAR ** pucFrame )
{
	*pucFrame = ( UCHAR * ) &ucMasterRTUSndBuf_2[MB_SER_PDU_PDU_OFF];
}

/* Set Modbus Master send PDU's buffer length.*/
void vMBMasterSetPDUSndLength_2( USHORT SendPDULength )
{
	usMasterSendPDULength = SendPDULength;
}

/* Get Modbus Master send PDU's buffer length.*/
USHORT usMBMasterGetPDUSndLength_2( void )
{
	return usMasterSendPDULength;
}

/* Set Modbus Master current timer mode.*/
void vMBMasterSetCurTimerMode_2( eMBMasterTimerMode eMBTimerMode )
{
	eMasterCurTimerMode = eMBTimerMode;
}

/* The master request is broadcast? */
BOOL xMBMasterRequestIsBroadcast_2( void ){
	return xFrameIsBroadcast;
}
#endif

