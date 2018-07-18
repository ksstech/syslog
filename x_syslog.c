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

#include	"hal_timer.h"

#include	"esp_log.h"

#include	<string.h>

#define	MODCODE					0x0104

#define	debugFLAG				0x0000
#define	debugTRACK				(debugFLAG & 0x0001)

// ########################################## macros ###############################################

#define	syslogSET_FG			"\033[0;%dm"
#define	syslogRST_FG			"\033[0m"

#define	configSYSLOG_BUFSIZE	2048

// ###################################### Global variables #########################################

sock_ctx_t	sSyslogCtx ;
uint32_t	SyslogMinSevLev = SL_SEV_DEBUG ;
char		SyslogBuffer[configSYSLOG_BUFSIZE] ;
SemaphoreHandle_t	SyslogMutex = NULL ;

char	SyslogColors[] = {	colourFG_RED,			// 0 = Emergency
							colourFG_RED,			// 1 = Alert
							colourFG_RED,			// 2 = Critical
							colourFG_RED,			// 3 = Error
							colourFG_MAGENTA,		// 4 = Warning
							colourFG_GREEN,			// 5 = Notice
							colourFG_WHITE,			// 6 = Info
							colourFG_YELLOW			// 7 = Debug
} ;

static char * SyslogLevel[8] = { "EMER", "ALERT", "CRIT", "ERROR", "WARN", "NOTICE", "INFO", "DEBUG" } ;

// ###################################### Public functions #########################################

/**
 * vSyslogInit()
 * \brief		Initialise the SysLog module
 * \param[in]	none
 * \param[out]	none
 * \return		none
 */
void	vSyslogInit(const char * pHostName) {
	memset(&sSyslogCtx, 0, sizeof(sock_ctx_t)) ;
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
	sSyslogCtx.d_ndebug			= 1 ;					// disable debug in x_sockets.c
	if (xNetGetHostByName(&sSyslogCtx) < erSUCCESS) {
		return ;
	}
	if ((sSyslogCtx.sd = socket(sSyslogCtx.sa_in.sin_family, sSyslogCtx.type, IPPROTO_IP)) < erSUCCESS) {
		return ;
	}
	if (connect(sSyslogCtx.sd, &sSyslogCtx.sa, sizeof(struct sockaddr_in)) < erSUCCESS) {
		close(sSyslogCtx.sd) ;
		return ;
	}
   	xNetSetNonBlocking(&sSyslogCtx, flagXNET_NONBLOCK) ;
   	vRtosSetStatus(flagNET_L5_SYSLOG) ;
   	IF_DEBUGPRINT_ERR(debugTRACK, "init") ;
}

/**
 * vSyslogDeInit()
 * \brief		De-initialise the SysLog module
 * \param[in]	none
 * \param[out]	none
 * \return		none
 */
void	vSyslogDeInit(void) {
	vRtosClearStatus(flagNET_L5_SYSLOG) ;
	close(sSyslogCtx.sd) ;
	sSyslogCtx.sd = -1 ;
	IF_DEBUGPRINT_ERR(debugTRACK, "deinit") ;
}

/**
 * vSyslogSetPriority() - sets the minimum priority level for logging to host
 * \brief		Lower value priority is actually higher ie more urgent
 * \brief		values <= Priority will be logged to host, > priority will go to console
 * \param[in]	Priority - value 0->7 indicating the threshold for host logging
 * \return		none
 */
void	vSyslogSetPriority(uint32_t Priority) { SyslogMinSevLev = Priority % 8 ; }

#if 0
/**
 * xvSyslog writes an RFC formatted message to syslog host
 * \brief		if syslog not up and running, write to stdout
 * \brief		avoid using malloc{} or similar since also called from error/crash handlers
 * \param[in]	Priority, ProcID and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal vprintf()
 * \param[out]	none
 * \return		number of characters displayed(if only to console) or send(if to server)
 */
int32_t	xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list vArgs) {
	myASSERT((Priority < 192) && INRANGE_MEM(MsgID) && INRANGE_MEM(format)) ;
	char *	ProcID ;
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
#if		(tskKERNEL_VERSION_MAJOR < 9)
		ProcID = pcTaskGetTaskName(NULL) ;							// FreeRTOS pre v9.0.0 uses long form function name
#else
		ProcID = pcTaskGetName(NULL) ;								// FreeRTOS v9.0.0 onwards uses short form function name
#endif
		myASSERT(INRANGE_SRAM(ProcID)) ;
	} else {
		ProcID = "preX" ;
	}

	cprintf_lock() ;
	int32_t xLen = cprintf(syslogSET_FG "%!R: %s %s ", SyslogColors[Priority & 0x07], halTIMER_ReadRunMicros(), ProcID, MsgID) ;
	xLen += vcprintf(format, vArgs) ;
	xLen += cprintf(syslogRST_FG "\n") ;
	cprintf_unlock() ;
	if (((Priority & 0x07) > SyslogMinSevLev) || (xRtosCheckStatus(flagNET_L5_SYSLOG) == 0)) {
		return xLen ;									// filter out higher levels, not going to syslog host...
	}

	xUtilLockResource(&SyslogMutex, portMAX_DELAY) ;
	xLen =	xsnprintf(SyslogBuffer, configSYSLOG_BUFSIZE, "<%u>1 %Z %s %s %s - %s ",
							Priority, &sTSZ, idSTA, ProcID, MsgID, SyslogLevel[Priority & 0x07]) ;

	xLen += xvsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, format, vArgs) ;
	sSyslogCtx.maxTx = xLen > sSyslogCtx.maxTx ? xLen : sSyslogCtx.maxTx ;

	// writing directly to socket, not via xNetWrite() to avoid recursing
	if (sendto(sSyslogCtx.sd, SyslogBuffer, xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in)) != xLen) {
		// Any error messages here ignored, obviously from IDF or lower levels...
		vRtosClearStatus(flagNET_L5_SYSLOG) ;
	}
	xUtilUnlockResource(&SyslogMutex) ;
	return xLen ;
}
#else

static	uint8_t	CurCRC ;
static	uint8_t	LstCRC ;
static	uint32_t RptCRC ;

/**
 * xvSyslog writes an RFC formatted message to syslog host
 * \brief		if syslog not up and running, write to stdout
 * \brief		avoid using malloc{} or similar since also called from error/crash handlers
 * \param[in]	Priority, ProcID and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal vprintf()
 * \param[out]	none
 * \return		number of characters displayed(if only to console) or send(if to server)
 */
int32_t	xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list vArgs) {
	myASSERT((Priority < 192) && INRANGE_MEM(MsgID) && INRANGE_MEM(format)) ;
	char *	ProcID ;
	if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
#if		(tskKERNEL_VERSION_MAJOR < 9)
		ProcID = pcTaskGetTaskName(NULL) ;							// FreeRTOS pre v9.0.0 uses long form function name
#else
		ProcID = pcTaskGetName(NULL) ;								// FreeRTOS v9.0.0 onwards uses short form function name
#endif
		myASSERT(INRANGE_SRAM(ProcID)) ;
	} else {
		ProcID = "preX" ;
	}

	xUtilLockResource(&SyslogMutex, portMAX_DELAY) ;
	// build the message into the buffer
	int32_t xLen1, xLen2 ;
	int32_t xLen = xsnprintf(SyslogBuffer, configSYSLOG_BUFSIZE, syslogSET_FG "%!R: %s ",
							SyslogColors[Priority & 0x07], halTIMER_ReadRunMicros(), ProcID) ;
	xLen1	= xLen ;				// save start of CRC area
	xLen += xsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, "%s ", MsgID) ;
	xLen += xvsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, format, vArgs) ;
	xLen2	= xLen ;				// save end of CRC area
	xLen += xsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, syslogRST_FG "\n") ;

	// calculate the CurCRC, then compare against LstCRC
	CurCRC = CalculaCheckSum((uint8_t *) &SyslogBuffer[xLen1], xLen2 - xLen1) ;
	if (CurCRC == LstCRC) {								// Same as previous message ?
		++RptCRC ;										// Yes, increment the repeat counter
		xUtilUnlockResource(&SyslogMutex) ;
		return xLen ;									// REPEAT message, not going to send...
	} else {
		if (RptCRC > 0) {								// if we have skipped messages
			cprintf("%d Identical messages skipped\n", RptCRC) ;
			RptCRC = 0 ;
		}
		LstCRC = CurCRC ;
	}

	cprintf(SyslogBuffer) ;								// new message so show it...

	if (((Priority & 0x07) > SyslogMinSevLev) || (xRtosCheckStatus(flagNET_L5_SYSLOG) == 0)) {
		xUtilUnlockResource(&SyslogMutex) ;
		return xLen ;									// filter out higher levels, not going to syslog host...
	}

	// Now start building the message in RFCxxxx format for host....
	xLen =	xsnprintf(SyslogBuffer, configSYSLOG_BUFSIZE, "<%u>1 %Z %s %s %s - %s ",
							Priority, &sTSZ, idSTA, ProcID, MsgID, SyslogLevel[Priority & 0x07]) ;

	xLen += xvsnprintf(&SyslogBuffer[xLen], configSYSLOG_BUFSIZE - xLen, format, vArgs) ;
	sSyslogCtx.maxTx = xLen > sSyslogCtx.maxTx ? xLen : sSyslogCtx.maxTx ;

	// writing directly to socket, not via xNetWrite() to avoid recursing
	if (sendto(sSyslogCtx.sd, SyslogBuffer, xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in)) != xLen) {
		// Any error messages here ignored, obviously from IDF or lower levels...
		vRtosClearStatus(flagNET_L5_SYSLOG) ;
	}
	xUtilUnlockResource(&SyslogMutex) ;
	return xLen ;
}
#endif

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

void	vSyslogReport(void) {
	if (xRtosCheckStatus(flagNET_L5_SYSLOG)) {
		xNetReport(&sSyslogCtx, __FUNCTION__, 0, 0, 0) ;
		SL_INFO("maxTX=%u  maxRX=%u", sSyslogCtx.maxTx, sSyslogCtx.maxRx) ;
	}
}
