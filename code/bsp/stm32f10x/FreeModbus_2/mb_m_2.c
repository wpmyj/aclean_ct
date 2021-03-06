/* 
 * FreeModbus Libary: A portable Modbus implementation for Modbus ASCII/RTU.
 * Copyright (C) 2013 Armink <armink.ztl@gmail.com>
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
 * File: $Id: mbrtu_m.c,v 1.60 2013/08/20 11:18:10 Armink Add Master Functions $
 */

/* ----------------------- System includes ----------------------------------*/
#include "stdlib.h"
#include "string.h"

/* ----------------------- Platform includes --------------------------------*/
#include "port_2.h"

/* ----------------------- Modbus includes ----------------------------------*/

#include "mb_2.h"
#include "mb_m_2.h"
#include "mbconfig_2.h"
#include "mbframe_2.h"
#include "mbproto_2.h"
#include "mbfunc_2.h"

#include "mbport_2.h"

#include "mbrtu_2.h"



#if MB_MASTER_RTU_ENABLED > 0 || MB_MASTER_ASCII_ENABLED > 0

#ifndef MB_PORT_HAS_CLOSE
#define MB_PORT_HAS_CLOSE 0
#endif

/* ----------------------- Static variables ---------------------------------*/

static UCHAR    ucMBMasterDestAddress;
static BOOL     xMBRunInMasterMode = FALSE;
static eMBMasterErrorEventType eMBMasterCurErrorType;

static enum
{
    STATE_ENABLED,
    STATE_DISABLED,
    STATE_NOT_INITIALIZED
} eMBState = STATE_NOT_INITIALIZED;

/* Functions pointer which are initialized in eMBInit( ). Depending on the
 * mode (RTU or ASCII) the are set to the correct implementations.
 * Using for Modbus Master,Add by Armink 20130813
 */
static peMBFrameSend peMBMasterFrameSendCur_2;
static pvMBFrameStart pvMBMasterFrameStartCur_2;
static pvMBFrameStop pvMBMasterFrameStopCur_2;
static peMBFrameReceive peMBMasterFrameReceiveCur_2;
static pvMBFrameClose pvMBMasterFrameCloseCur_2;

/* Callback functions required by the porting layer. They are called when
 * an external event has happend which includes a timeout or the reception
 * or transmission of a character.
 * Using for Modbus Master,Add by Armink 20130813
 */
BOOL( *pxMBMasterFrameCBByteReceived_2 ) ( void );
BOOL( *pxMBMasterFrameCBTransmitterEmpty_2 ) ( void );
BOOL( *pxMBMasterPortCBTimerExpired_2 ) ( void );

BOOL( *pxMBMasterFrameCBReceiveFSMCur_2 ) ( void );
BOOL( *pxMBMasterFrameCBTransmitFSMCur_2 ) ( void );

/* An array of Modbus functions handlers which associates Modbus function
 * codes with implementing functions.
 */
static xMBFunctionHandler xMasterFuncHandlers[MB_FUNC_HANDLERS_MAX] = {
#if MB_FUNC_OTHER_REP_SLAVEID_ENABLED > 0
	//TODO Add Master function define
    {MB_FUNC_OTHER_REPORT_SLAVEID, eMBFuncReportSlaveID_2},
#endif
#if MB_FUNC_READ_INPUT_ENABLED > 0
    {MB_FUNC_READ_INPUT_REGISTER, eMBMasterFuncReadInputRegister_2},
#endif
#if MB_FUNC_READ_HOLDING_ENABLED > 0
    {MB_FUNC_READ_HOLDING_REGISTER, eMBMasterFuncReadHoldingRegister_2},
#endif
#if MB_FUNC_WRITE_MULTIPLE_HOLDING_ENABLED > 0
    {MB_FUNC_WRITE_MULTIPLE_REGISTERS, eMBMasterFuncWriteMultipleHoldingRegister_2},
#endif
#if MB_FUNC_WRITE_HOLDING_ENABLED > 0
    {MB_FUNC_WRITE_REGISTER, eMBMasterFuncWriteHoldingRegister_2},
#endif
#if MB_FUNC_READWRITE_HOLDING_ENABLED > 0
    {MB_FUNC_READWRITE_MULTIPLE_REGISTERS, eMBMasterFuncReadWriteMultipleHoldingRegister_2},
#endif
#if MB_FUNC_READ_COILS_ENABLED > 0
    {MB_FUNC_READ_COILS, eMBMasterFuncReadCoils_2},
#endif
#if MB_FUNC_WRITE_COIL_ENABLED > 0
    {MB_FUNC_WRITE_SINGLE_COIL, eMBMasterFuncWriteCoil_2},
#endif
#if MB_FUNC_WRITE_MULTIPLE_COILS_ENABLED > 0
    {MB_FUNC_WRITE_MULTIPLE_COILS, eMBMasterFuncWriteMultipleCoils_2},
#endif
#if MB_FUNC_READ_DISCRETE_INPUTS_ENABLED > 0
    {MB_FUNC_READ_DISCRETE_INPUTS, eMBMasterFuncReadDiscreteInputs_2},
#endif
};

/* ----------------------- Start implementation -----------------------------*/
eMBErrorCode
eMBMasterInit_2( eMBMode eMode, UCHAR ucPort, ULONG ulBaudRate, eMBParity eParity )
{
    eMBErrorCode    eStatus = MB_ENOERR;

	switch (eMode)
	{
#if MB_MASTER_RTU_ENABLED > 0
	case MB_RTU:
		pvMBMasterFrameStartCur_2 = eMBMasterRTUStart_2;
		pvMBMasterFrameStopCur_2 = eMBMasterRTUStop_2;
		peMBMasterFrameSendCur_2 = eMBMasterRTUSend_2;
		peMBMasterFrameReceiveCur_2 = eMBMasterRTUReceive_2;
		pvMBMasterFrameCloseCur_2 = MB_PORT_HAS_CLOSE ? vMBMasterPortClose_2 : NULL;
		pxMBMasterFrameCBByteReceived_2 = xMBMasterRTUReceiveFSM_2;
		pxMBMasterFrameCBTransmitterEmpty_2 = xMBMasterRTUTransmitFSM_2;
		pxMBMasterPortCBTimerExpired_2 = xMBMasterRTUTimerExpired_2;

		eStatus = eMBMasterRTUInit_2(ucPort, ulBaudRate, eParity);
		break;
#endif


	default:
		eStatus = MB_EINVAL;
		break;
	}

	if (eStatus == MB_ENOERR)
	{
		if (!xMBMasterPortEventInit_2())
		{
			/* port dependent event module initalization failed. */
			eStatus = MB_EPORTERR;
		}
		else
		{
			eMBState = STATE_DISABLED;
		}
		/* initialize the OS resource for modbus master. */
		vMBMasterOsResInit_2();
	}
	return eStatus;
}

eMBErrorCode
eMBMasterClose_2( void )
{
    eMBErrorCode    eStatus = MB_ENOERR;

    if( eMBState == STATE_DISABLED )
    {
        if( pvMBMasterFrameCloseCur_2 != NULL )
        {
            pvMBMasterFrameCloseCur_2(  );
        }
    }
    else
    {
        eStatus = MB_EILLSTATE;
    }
    return eStatus;
}

eMBErrorCode
eMBMasterEnable_2( void )
{
    eMBErrorCode    eStatus = MB_ENOERR;

    if( eMBState == STATE_DISABLED )
    {
        /* Activate the protocol stack. */
        pvMBMasterFrameStartCur_2(  );
        eMBState = STATE_ENABLED;
    }
    else
    {
        eStatus = MB_EILLSTATE;
    }
    return eStatus;
}

eMBErrorCode
eMBMasterDisable_2( void )
{
    eMBErrorCode    eStatus;

    if( eMBState == STATE_ENABLED )
    {
        pvMBMasterFrameStopCur_2(  );
        eMBState = STATE_DISABLED;
        eStatus = MB_ENOERR;
    }
    else if( eMBState == STATE_DISABLED )
    {
        eStatus = MB_ENOERR;
    }
    else
    {
        eStatus = MB_EILLSTATE;
    }
    return eStatus;
}

eMBErrorCode
eMBMasterPoll_2( void )
{
    static UCHAR   *ucMBFrame;
    static UCHAR    ucRcvAddress;
    static UCHAR    ucFunctionCode;
    static USHORT   usLength;
    static eMBException eException;

    int             i , j;
    eMBErrorCode    eStatus = MB_ENOERR;
    eMBMasterEventType    eEvent;
    eMBMasterErrorEventType errorType;

    /* Check if the protocol stack is ready. */
    if( eMBState != STATE_ENABLED )
    {
        return MB_EILLSTATE;
    }

    /* Check if there is a event available. If not return control to caller.
     * Otherwise we will handle the event. */
    if( xMBMasterPortEventGet_2( &eEvent ) == TRUE )
    {
        switch ( eEvent )
        {
        case EV_MASTER_READY:
            break;

        case EV_MASTER_FRAME_RECEIVED:
			eStatus = peMBMasterFrameReceiveCur_2( &ucRcvAddress, &ucMBFrame, &usLength );
			/* Check if the frame is for us. If not ,send an error process event. */
			if ( ( eStatus == MB_ENOERR ) && ( ucRcvAddress == ucMBMasterGetDestAddress_2() ) )
			{
				( void ) xMBMasterPortEventPost_2( EV_MASTER_EXECUTE );
			}
			else
			{
				vMBMasterSetErrorType_2(EV_ERROR_RECEIVE_DATA);
				( void ) xMBMasterPortEventPost_2( EV_MASTER_ERROR_PROCESS );
			}
			break;

        case EV_MASTER_EXECUTE:
            ucFunctionCode = ucMBFrame[MB_PDU_FUNC_OFF];
            eException = MB_EX_ILLEGAL_FUNCTION;
            /* If receive frame has exception .The receive function code highest bit is 1.*/
            if(ucFunctionCode >> 7) {
            	eException = (eMBException)ucMBFrame[MB_PDU_DATA_OFF];
            }
			else
			{
				for (i = 0; i < MB_FUNC_HANDLERS_MAX; i++)
				{
					/* No more function handlers registered. Abort. */
					if (xMasterFuncHandlers[i].ucFunctionCode == 0)	{
						break;
					}
					else if (xMasterFuncHandlers[i].ucFunctionCode == ucFunctionCode) {
						vMBMasterSetCBRunInMasterMode_2(TRUE);
						/* If master request is broadcast,
						 * the master need execute function for all slave.
						 */
						if ( xMBMasterRequestIsBroadcast_2() ) {
							usLength = usMBMasterGetPDUSndLength_2();
							for(j = 1; j <= MB_MASTER_TOTAL_SLAVE_NUM; j++){
								vMBMasterSetDestAddress_2(j);
								eException = xMasterFuncHandlers[i].pxHandler(ucMBFrame, &usLength);
							}
						}
						else {
							eException = xMasterFuncHandlers[i].pxHandler(ucMBFrame, &usLength);
						}
						vMBMasterSetCBRunInMasterMode_2(FALSE);
						break;
					}
				}
			}
            /* If master has exception ,Master will send error process.Otherwise the Master is idle.*/
            if (eException != MB_EX_NONE) {
            	vMBMasterSetErrorType_2(EV_ERROR_EXECUTE_FUNCTION);
            	( void ) xMBMasterPortEventPost_2( EV_MASTER_ERROR_PROCESS );
            }
            else {
            	vMBMasterCBRequestScuuess_2( );
            	vMBMasterRunResRelease_2( );
            }
            break;

        case EV_MASTER_FRAME_SENT:
        	/* Master is busy now. */
        	vMBMasterGetPDUSndBuf_2( &ucMBFrame );
			eStatus = peMBMasterFrameSendCur_2( ucMBMasterGetDestAddress_2(), ucMBFrame, usMBMasterGetPDUSndLength_2() );
            break;

        case EV_MASTER_ERROR_PROCESS:
        	/* Execute specified error process callback function. */
			errorType = eMBMasterGetErrorType_2();
			vMBMasterGetPDUSndBuf_2( &ucMBFrame );
			switch (errorType) {
			case EV_ERROR_RESPOND_TIMEOUT:
				vMBMasterErrorCBRespondTimeout_2(ucMBMasterGetDestAddress_2(),
						ucMBFrame, usMBMasterGetPDUSndLength_2());
				break;
			case EV_ERROR_RECEIVE_DATA:
				vMBMasterErrorCBReceiveData_2(ucMBMasterGetDestAddress_2(),
						ucMBFrame, usMBMasterGetPDUSndLength_2());
				break;
			case EV_ERROR_EXECUTE_FUNCTION:
				vMBMasterErrorCBExecuteFunction_2(ucMBMasterGetDestAddress_2(),
						ucMBFrame, usMBMasterGetPDUSndLength_2());
				break;
			}
			vMBMasterRunResRelease_2();
        	break;
        }
    }
    return MB_ENOERR;
}

/* Get whether the Modbus Master is run in master mode.*/
BOOL xMBMasterGetCBRunInMasterMode_2( void )
{
	return xMBRunInMasterMode;
}
/* Set whether the Modbus Master is run in master mode.*/
void vMBMasterSetCBRunInMasterMode_2( BOOL IsMasterMode )
{
	xMBRunInMasterMode = IsMasterMode;
}
/* Get Modbus Master send destination address. */
UCHAR ucMBMasterGetDestAddress_2( void )
{
	return ucMBMasterDestAddress;
}
/* Set Modbus Master send destination address. */
void vMBMasterSetDestAddress_2( UCHAR Address )
{
	ucMBMasterDestAddress = Address;
}
/* Get Modbus Master current error event type. */
eMBMasterErrorEventType eMBMasterGetErrorType_2( void )
{
	return eMBMasterCurErrorType;
}
/* Set Modbus Master current error event type. */
void vMBMasterSetErrorType_2( eMBMasterErrorEventType errorType )
{
	eMBMasterCurErrorType = errorType;
}



#endif
