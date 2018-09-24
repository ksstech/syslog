/*
 * Copyright 2014-18 Andre M Maree / KSS Technologies (Pty) Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

/*
 * x_syslog.c
 *
 ******************************************************************************************************************
 SYSLOG-MSG = HEADER SP STRUCTURED-DATA [SP MSG]

 HEADER = PRI VERSION SP TIMESTAMP SP HOSTNAME SP APP-NAME SP PROCID SP MSGID

 PRI = "<" PRIVAL ">"
	PRIVAL = 1*3DIGIT ; range 0 .. 191

 VERSION = NONZERO-DIGIT 0*2DIGIT

 HOSTNAME = NILVALUE / 1*255PRINTUSASCII

 APP-NAME = NILVALUE / 1*48PRINTUSASCII

 PROCID = NILVALUE / 1*128PRINTUSASCII

 MSGID = NILVALUE / 1*32PRINTUSASCII

 TIMESTAMP = NILVALUE / FULL-DATE "T" FULL-TIME

 FULL-DATE = DATE-FULLYEAR "-" DATE-MONTH "-" DATE-MDAY
	DATE-FULLYEAR = 4DIGIT
	DATE-MONTH = 2DIGIT ; 01-12
	DATE-MDAY = 2DIGIT ; 01-28, 01-29, 01-30, 01-31 based on ; month/year
 FULL-TIME = PARTIAL-TIME TIME-OFFSET
	PARTIAL-TIME = TIME-HOUR ":" TIME-MINUTE ":" TIME-SECOND [TIME-SECFRAC]
	TIME-HOUR = 2DIGIT ; 00-23
	TIME-MINUTE = 2DIGIT ; 00-59
	TIME-SECOND = 2DIGIT ; 00-59
	TIME-SECFRAC = "." 1*6DIGIT
	TIME-OFFSET = "Z" / TIME-NUMOFFSET
	TIME-NUMOFFSET = ("+" / "-") TIME-HOUR ":" TIME-MINUTE

 STRUCTURED-DATA = NILVALUE / 1*SD-ELEMENT
	SD-ELEMENT = "[" SD-ID *(SP SD-PARAM) "]"
	SD-PARAM = PARAM-NAME "=" %d34 PARAM-VALUE %d34
	SD-ID = SD-NAME
	PARAM-NAME = SD-NAME
	PARAM-VALUE = UTF-8-STRING ; characters ’"’, ’\’ and ; ’]’ MUST be escaped.
	SD-NAME = 1*32PRINTUSASCII ; except ’=’, SP, ’]’, %d34 (")

 MSG = MSG-ANY / MSG-UTF8
 MSG-ANY = *OCTET ; not starting with BOM
 MSG-UTF8 = BOM UTF-8-STRING
 BOM = %xEF.BB.BF

UTF-8-STRING = *OCTET ; UTF-8 string as specified ; in RFC 3629
 OCTET = %d00-255
 SP = %d32
 PRINTUSASCII = %d33-126
 NONZERO-DIGIT = %d49-57
 DIGIT = %d48 / NONZERO-DIGIT
 NILVALUE = "-"

 ******************************************************************************************************************
 * Theory of operation.
 *
 *	#1	ALL messages are sent to the console (if present)
 *	#2	Only messages with SEVerity <= SyslogMinSevLev will be logged to the syslog server
 *
 *	To minimise the impact on application size the SL_xxxx macros must be used to in/exclude levels of info.
 * 		SL_DBG() to control inclusion and display of DEBUG type information
 *		SL_INFO() to control the next level of information verbosity
 *		SL_NOT() to control information inclusion/display of important events, NOT errors
 *		SL_WARN() to inform on concerns such as values closely approaching threshold
 *		SL_ERR() for errors that the system can/will recover from automatically
 *		SL_CRIT/ALRT() ?????
 *		SL_EMER() should be reserved for unrecoverable errors that should result in a system restart
 *
 */

#include	"FreeRTOS_Support.h"

#include	"x_debug.h"
#include	"x_syslog.h"
#include	"x_time.h"
#include	"x_terminal.h"
#include	"x_errors_events.h"
#include	"x_retarget.h"
#include	"x_utilities.h"
#include	"crc.h"

#include	"hal_network.h"
#include	"hal_timer.h"

#include	"esp_log.h"

#include	<string.h>

#define	debugFLAG				0x0000
#define	debugPARAM				(debugFLAG & 0x0001)
#define	debugTRACK				(debugFLAG & 0x0002)

// ########################################## macros ###############################################

#define	syslogSET_FG			"\033[0;%dm"
#define	syslogRST_FG			"\033[0m"

#define	configSYSLOG_BUFSIZE	2048

// ###################################### Global variables #########################################

static	sock_ctx_t	sSyslogCtx ;
static	char		SyslogBuffer[configSYSLOG_BUFSIZE] ;
SemaphoreHandle_t	SyslogMutex ;
static	uint8_t		CurCRC, LstCRC ;
static	uint32_t 	CurRpt, MsgCnt, TotRpt ;

static	uint32_t	SyslogMinSevLev = SL_SEV_DEBUG ;
static char * SyslogLevel[8] = { "EMER", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG" } ;
char	SyslogColors[8] = {
// 0 = Emergency	1 = Alert	2 = Critical	3 = Error		4 = Warning		5 = Notice		6 = Info		7 = Debug
	colourFG_RED, colourFG_RED, colourFG_RED, colourFG_RED, colourFG_MAGENTA, colourFG_GREEN,	colourFG_WHITE,	colourFG_YELLOW } ;

// ###################################### Public functions #########################################

/**
 * vSyslogInit()
 * \brief		Initialise the SysLog module
 * \param[in]	none
 * \param[out]	none
 * \return		none
 */
void	vSyslogInit(const char * pHostName) {
	IF_myASSERT(debugPARAM, pHostName) ;
	sSyslogCtx.pHost			= pHostName ;
	sSyslogCtx.sa_in.sin_family = AF_INET ;
#if		(buildSYSLOG_USE_UDP == 1)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_UDP) ;
#elif		(buildSYSLOG_USE_TCP == 1) && (IP_PORT_SYSLOG_TLS == 0)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_TCP) ;
#elif		(buildSYSLOG_USE_TCP == 1) && (IP_PORT_SYSLOG_TLS == 1)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_TLS) ;
#endif
	sSyslogCtx.type				= SOCK_DGRAM ;
	sSyslogCtx.d_flags			= 0 ;
	sSyslogCtx.d_ndebug			= 1 ;					// disable debug in x_sockets.c
	if (xNetGetHostByName(&sSyslogCtx) < erSUCCESS) {
		return ;
	}
	if ((sSyslogCtx.sd = socket(sSyslogCtx.sa_in.sin_family, sSyslogCtx.type, IPPROTO_IP)) < erSUCCESS) {
		sSyslogCtx.sd = -1 ;
		return ;
	}
	if (connect(sSyslogCtx.sd, &sSyslogCtx.sa, sizeof(struct sockaddr_in)) < erSUCCESS) {
		close(sSyslogCtx.sd) ;
		sSyslogCtx.sd = -1 ;
		return ;
	}
   	xNetSetNonBlocking(&sSyslogCtx, flagXNET_NONBLOCK) ;
   	vRtosSetStatus(flagNET_SYSLOG) ;
   	IF_CPRINT(debugTRACK, "init") ;
}

/**
 * vSyslogDeInit()
 * \brief		De-initialise the SysLog module
 * \param[in]	none
 * \param[out]	none
 * \return		none
 */
void	vSyslogDeInit(void) {
	vRtosClearStatus(flagNET_SYSLOG) ;
	close(sSyslogCtx.sd) ;
	sSyslogCtx.sd = -1 ;
	IF_CPRINT(debugTRACK, "deinit") ;
}

/**
 * vSyslogSetPriority() - sets the minimum priority level for logging to host
 * \brief		Lower value priority is actually higher ie more urgent
 * \brief		values <= Priority will be logged to host, > priority will go to console
 * \param[in]	Priority - value 0->7 indicating the threshold for host logging
 * \return		none
 */
void	vSyslogSetPriority(uint32_t Priority) { SyslogMinSevLev = Priority % 8 ; }

/**
 * xvSyslog writes an RFC formatted message to syslog host
 * \brief		if syslog not up and running, write to stdout
 * \brief		avoid using malloc or similar since also called from error/crash handlers
 * \param[in]	Priority, ProcID and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal vprintf()
 * \param[out]	none
 * \return		number of characters displayed(if only to console) or send(if to server)
 */
int32_t	xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list vArgs) {
	IF_myASSERT(debugPARAM, (Priority < 192) && INRANGE_MEM(MsgID) && INRANGE_MEM(format)) ;
	int32_t	FRflag ;
	char *	ProcID ;
	// Step 1: handle state of scheduler, and obtain the task name and secure exclusive access to buffer
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		FRflag = 1 ;
#if		(tskKERNEL_VERSION_MAJOR < 9)
		ProcID = pcTaskGetTaskName(NULL) ;							// FreeRTOS pre v9.0.0 uses long form function name
#else
		ProcID = pcTaskGetName(NULL) ;								// FreeRTOS v9.0.0 onwards uses short form function name
#endif
		IF_myASSERT(debugPARAM, INRANGE_SRAM(ProcID)) ;
	} else {
		FRflag = 0 ;
		ProcID = "preX" ;
	}

	if (FRflag) {
		xUtilLockResource(&SyslogMutex, portMAX_DELAY) ;
	}

	// Step 2: build the console formatted message into the buffer
	uint64_t LogTime = halTIMER_ReadRunMicros() ;
	int32_t xLen = xsnprintf(SyslogBuffer, configSYSLOG_BUFSIZE, syslogSET_FG "%!R: %s ", SyslogColors[Priority & 0x07], LogTime, ProcID) ;
	int32_t yLen = xLen ;							// save start of CRC area
	xLen += xsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, "%s ", MsgID) ;
	xLen += xvsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, format, vArgs) ;
	int32_t zLen = xLen ;							// save end of CRC area
	xLen += xsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, syslogRST_FG "\n") ;

	// Step 3: Check if this is a sequential repeat message, if so, count but dont display
	CurCRC = CalculaCheckSum((uint8_t *) &SyslogBuffer[yLen], zLen - yLen) ;
	if (CurCRC == LstCRC) {								// CRC same as previous message ?
		++CurRpt ;										// Yes, increment the repeat counter
		++TotRpt ;
		if (FRflag) {
			xUtilUnlockResource(&SyslogMutex) ;
		}
		return xLen ;									// REPEAT message, not going to send...
	}

	// Step 4: we have a new/different message, handle earlier suppressed duplicates
	if (CurRpt > 0) {									// if we have skipped messages
	#define	syslogSKIP_FORMAT	"%!R: %s xvSyslog Identical messages (%d) skipped\n"
		if (FRflag) {									// indicate number of repetitions in the log...
			xprintf(syslogSKIP_FORMAT, LogTime, ProcID, CurRpt) ;
		} else {
			cprintf_noblock(syslogSKIP_FORMAT, LogTime, ProcID, CurRpt) ;
		}
		CurRpt = 0 ;
	}

	// Step 5: show the new message to the console...
	LstCRC = CurCRC ;
	if (FRflag) {
		xprintf(SyslogBuffer) ;
	} else {
		cprintf_noblock(SyslogBuffer) ;
	}

	// Step 6: filter out reasons why message should not go to syslog host...
	if ((ipSTA == 0) || (xRtosCheckStatus(flagNET_L3) == 0) || (FRflag == 0) || ((Priority & 0x07) > SyslogMinSevLev)) {
		if (FRflag) {
			xUtilUnlockResource(&SyslogMutex) ;
		}
		return xLen ;
	}

	// Step 7: If not connected to host, try to connect
	if (xRtosCheckStatus(flagNET_SYSLOG) == 0) {		// syslog not connected ?
		vSyslogInit(configSYSLOG_HOSTNAME) ;			// try to connect...
		if (xRtosCheckStatus(flagNET_SYSLOG) == 0) {	// successful?
			xUtilUnlockResource(&SyslogMutex) ;			// no, free up locked buffer
			return xLen ;								// and return
		}
	}

	// Step 8: Now start building the message in RFCxxxx format for host....
	xLen =	xsnprintf(SyslogBuffer, configSYSLOG_BUFSIZE, "<%u>1 %+Z %s %s %s - %s ",
							Priority, &sTSZ, idSTA, ProcID, MsgID, SyslogLevel[Priority & 0x07]) ;

	xLen += xvsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, format, vArgs) ;
	sSyslogCtx.maxTx = (xLen > sSyslogCtx.maxTx) ? xLen : sSyslogCtx.maxTx ;

	// Step 9: write message directly to socket, not via xNetWrite() to avoid recursing
	if (sendto(sSyslogCtx.sd, SyslogBuffer, xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in)) != xLen) {
		vSyslogDeInit() ;
	} else {
		++MsgCnt ;
	}
	xUtilUnlockResource(&SyslogMutex) ;
	return xLen ;
}

/**
 * xSyslog writes an RFC formatted message to syslog host
 * \brief		if syslog not up and running, write to stdout
 * \brief		avoid using malloc{} or similar since also called from error/crash handlers
 * \param[in]	Priority, ProcID and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal printf()
 * \param[out]	none
 * \return		number of characters displayed(if only to console) or send(if to server)
 */
int32_t	xSyslog(uint32_t Priority, const char * MsgID, const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
	int32_t iRV = xvSyslog(Priority, MsgID, format, vaList) ;
    va_end(vaList) ;
    return iRV ;
}

void	vSyslogReport(int32_t Handle) {
	if (xRtosCheckStatus(flagNET_SYSLOG)) {
		xNetReport(Handle, &sSyslogCtx, __FUNCTION__, 0, 0, 0) ;
		xdprintf(Handle, "\t\t\tmaxTX=%u  CurRpt=%d  TotRpt=%d  TxMsg=%d\n", sSyslogCtx.maxTx, CurRpt, TotRpt, MsgCnt) ;
	}
}
