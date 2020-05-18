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
 * syslog.c
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
	PARAM-VALUE = UTF-8-STRING ; characters �"�, �\� and ; �]� MUST be escaped.
	SD-NAME = 1*32PRINTUSASCII ; except �=�, SP, �]�, %d34 (")

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

#include	"syslog.h"
#include	"FreeRTOS_Support.h"
#include	"printfx.h"
#include	"x_sockets.h"
#include	"x_errors_events.h"
#include	"x_retarget.h"

#include	"hal_debug.h"
#include	"hal_network.h"
#include	"hal_timer.h"
#include	"hal_nvic.h"

#if		(ESP32_PLATFORM == 1)
	#include	"esp_log.h"
	#include	"esp32/rom/crc.h"						// ESP32 ROM routine
#else
	#include	"crc-barr.h"							// Barr group CRC
#endif

#include	<string.h>

#define	debugFLAG				0x0000

#define	debugTRACK				(debugFLAG & 0x2000)
#define	debugPARAM				(debugFLAG & 0x4000)
#define	debugRESULT				(debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ##############################

#define	syslogBUFSIZE			2048

#define	syslogUSE_UDP			1
#define	syslogUSE_TCP			0
#define	syslogUSE_TLS			0

/**
 * Compile time system checks
 */
#if		MORETHAN1of3(syslogUSE_UDP, syslogUSE_TCP, syslogUSE_TLS)
	#error	"More than 1 option of UDP vs TCP selected !!!"
#endif

// ###################################### Global variables #########################################

/* In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
 * could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
 * seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
 * level should be set to NOTICE. */
#if		(ESP32_PLATFORM == 1)
static	uint32_t	SyslogMinSevLev = CONFIG_LOG_DEFAULT_LEVEL + 2 ;		// align with ESP-IDF levels
#else
static	uint32_t	SyslogMinSevLev = SL_SEV_WARNING ;
#endif

static	netx_t		sSyslogCtx ;
static	char		SyslogBuffer[syslogBUFSIZE] ;
SemaphoreHandle_t	SyslogMutex ;
static	char		SyslogColors[8] = {
// 0 = Emergency	1 = Alert	2 = Critical	3 = Error		4 = Warning		5 = Notice		6 = Info		7 = Debug
	colourFG_RED, colourFG_RED, colourBG_BLUE, colourFG_MAGENTA, colourFG_YELLOW, colourFG_CYAN,	colourFG_GREEN,	colourFG_WHITE } ;
static	uint64_t	* ptRunTime, * ptUTCTime ;
static	uint32_t 	RptCRC, RptCNT ;
static	uint64_t	RptRUN, RptUTC ;
static	uint8_t		RptPRI ;

// ###################################### Public functions #########################################

/**
 * xSyslogInit() - Initialise the SysLog module
 * \param[in]	host name to log to
 * \param[in]	pointer to uSec runtime counter
 * \param[in]	pointer to uSec UTC time value
 * \return		true if connection successful
 */
int32_t	IRAM_ATTR xSyslogInit(const char * pcHostName, uint64_t * pRunTime, uint64_t * pUTCTime) {
	IF_myASSERT(debugPARAM, pcHostName && INRANGE_SRAM(pRunTime) && INRANGE_SRAM(pUTCTime)) ;
	if (sSyslogCtx.pHost != NULL || sSyslogCtx.sd > 0) {
		vSyslogDisConnect() ;
		memset(&sSyslogCtx, 0, sizeof(sSyslogCtx)) ;
	}
	ptRunTime = pRunTime ;
	ptUTCTime = pUTCTime ;
	sSyslogCtx.pHost = pcHostName ;
	return xSyslogConnect() ;
}

/**
 * vSyslogConnect() - establish connection to the selected syslog host
 * \return		true if successful else false
 */
int32_t	IRAM_ATTR xSyslogConnect(void) {
	IF_myASSERT(debugPARAM, sSyslogCtx.pHost) ;
	if (bRtosCheckStatus(flagLX_STA) == false) {
		return false ;
	}
	sSyslogCtx.sa_in.sin_family = AF_INET ;
#if		(syslogUSE_UDP == 1)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_UDP) ;
#elif		(syslogUSE_TCP == 1) && (IP_PORT_SYSLOG_TLS == 0)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_TCP) ;
#elif		(syslogUSE_TCP == 1) && (IP_PORT_SYSLOG_TLS == 1)
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_TLS) ;
#endif
	sSyslogCtx.type				= SOCK_DGRAM ;
	sSyslogCtx.d_flags			= 0 ;
	sSyslogCtx.d_ndebug			= 1 ;					// disable debug in x_sockets.c
	if (xNetGetHostByName(&sSyslogCtx) < erSUCCESS) {
	   	IF_CTRACK(debugTRACK, "FAIL Hostname") ;
		return false ;
	}
	if ((sSyslogCtx.sd = socket(sSyslogCtx.sa_in.sin_family, sSyslogCtx.type, IPPROTO_IP)) < erSUCCESS) {
		sSyslogCtx.sd = -1 ;
	   	IF_CTRACK(debugTRACK, "FAIL socket") ;
		return false ;
	}
	if (connect(sSyslogCtx.sd, &sSyslogCtx.sa, sizeof(struct sockaddr_in)) < erSUCCESS) {
		close(sSyslogCtx.sd) ;
		sSyslogCtx.sd = -1 ;
	   	IF_CTRACK(debugTRACK, "FAIL connect") ;
		return false ;
	}
   	xNetSetNonBlocking(&sSyslogCtx, flagXNET_NONBLOCK) ;
   	xRtosSetStatus(flagNET_SYSLOG) ;
   	IF_CTRACK(debugTRACK, "connect") ;
   	return true ;
}

/**
 * vSyslogDisConnect()	De-initialise the SysLog module
 */
void	IRAM_ATTR vSyslogDisConnect(void) {
	xRtosClearStatus(flagNET_SYSLOG) ;
	close(sSyslogCtx.sd) ;
	sSyslogCtx.sd = -1 ;
	IF_CTRACK(debugTRACK, "disconnect") ;
}

/**
 * vSyslogSetPriority() - sets the minimum priority level for logging to host
 * \brief		Lower value priority is actually higher ie more urgent
 * \brief		values <= Priority will be logged to host, > priority will go to console
 * \param[in]	Priority - value 0->7 indicating the threshold for host logging
 * \return		none
 */
void	vSyslogSetPriority(uint32_t Priority) { SyslogMinSevLev = Priority % 8 ; }

bool	IRAM_ATTR bSyslogCheckStatus(uint8_t MsgPRI) {
   	IF_CTRACK(debugTRACK, "MsgPRI=%d  SLminSL=%d", MsgPRI % 8, SyslogMinSevLev) ;
	if (bRtosCheckStatus(flagLX_STA) == false || (MsgPRI % 8) > SyslogMinSevLev) {
		return false ;
	}

	if (bRtosCheckStatus(flagNET_SYSLOG) == false) {
	   	IF_CTRACK(debugTRACK, "connecting...") ;
		return xSyslogConnect() ;
	}
	return true ;
}

int32_t	IRAM_ATTR xSyslogSendMessage(char * pcBuffer, int32_t xLen) {
	// write directly to socket, not via xNetWrite(), to avoid recursing
	int32_t	iRV = sendto(sSyslogCtx.sd, pcBuffer, xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in)) ;
	if (iRV == xLen) {
		sSyslogCtx.maxTx = (xLen > sSyslogCtx.maxTx) ? xLen : sSyslogCtx.maxTx ;
	} else {
		vSyslogDisConnect() ;
	}
	return iRV ;
}

/**
 * xvSyslog writes an RFC formatted message to syslog host
 * \brief		write to stdout & syslog host (if up and running)
 * \param[in]	Priority and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal vprintf()
 * \param[out]	none
 * \return		number of characters sent to server
 */
int32_t	IRAM_ATTR xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list vArgs) {
	IF_myASSERT(debugPARAM, INRANGE_MEM(MsgID) && INRANGE_MEM(format)) ;

	// Step 0: handle state of scheduler and obtain the task name
	bool	FRflag ;
	char *	ProcID ;
	xRtosSemaphoreTake(&SyslogMutex, portMAX_DELAY) ;
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		FRflag = true ;
#if	(tskKERNEL_VERSION_MAJOR < 9)
		ProcID = pcTaskGetTaskName(NULL) ;				// FreeRTOS pre v9.0.0 uses long form function name
#else
		ProcID = pcTaskGetName(NULL) ;					// FreeRTOS v9.0.0 onwards uses short form function name
#endif
		IF_myASSERT(debugPARAM, INRANGE_SRAM(ProcID)) ;
		char * pcTmp  = ProcID ;
		while (*pcTmp) {
			if (*pcTmp == CHR_SPACE) {
				*pcTmp = CHR_UNDERSCORE ;
			}
			++pcTmp ;
		}
	} else {
		FRflag = false ;
		ProcID = (char *) "preX" ;
	}

	// Step 1: setup time, priority and related variables
	uint8_t		MsgPRI	= Priority % 256 ;
	uint64_t	MsgRUN, MsgUTC ;
	if (ptRunTime == NULL) {
		MsgRUN	=	MsgUTC	= esp_log_timestamp() * MICROS_IN_MILLISEC ;
	} else {
		MsgRUN	= *ptRunTime ;
		MsgUTC	= *ptUTCTime ;
	}
#if	(ESP32_PLATFORM == 1) && !defined(CONFIG_FREERTOS_UNICORE)
	int32_t		McuID	= xPortGetCoreID() ;
#else
	int32_t		McuID	= 0 ;							// default in case not ESP32 or scheduler not running
#endif

	// Step 2: build the console formatted message into the buffer
	int32_t xLen = snprintfx(SyslogBuffer, syslogBUFSIZE, "%s %s ", ProcID, MsgID) ;
	xLen += vsnprintfx(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;

	// Calc CRC to check for repeat message, handle accordingly
#if		(ESP32_PLATFORM == 1)							// use ROM based CRC lookup table
	uint32_t MsgCRC = crc32_le(0, (uint8_t *) SyslogBuffer, xLen) ;
#else													// use fastest of external libraries
	uint32_t MsgCRC = crcSlow((uint8_t *) SyslogBuffer, xLen) ;
#endif

	if (MsgCRC == RptCRC && MsgPRI == RptPRI) {			// CRC & PRI same as previous message ?
		++RptCNT ;										// Yes, increment the repeat counter
		RptRUN = MsgRUN ;								// save timestamps of latest repeat
		RptUTC = MsgUTC ;
		xLen = 0 ;										// nothing was sent via network

	} else {											// new/different message, handle suppressed duplicates
		RptCRC = MsgCRC ;
		RptPRI = MsgPRI ;
		if (RptCNT > 0) {								// if we have skipped messages
			PRINT("%C%!R: #%d Last of %d (skipped) Identical messages%C\n", xpfSGR(attrRESET, SyslogColors[RptPRI & 0x07],0,0), RptRUN, McuID, RptCNT, attrRESET) ;
			// build & send skipped message to host
			if (FRflag && bSyslogCheckStatus(MsgPRI)) {
				xLen =	snprintfx(SyslogBuffer, syslogBUFSIZE, "<%u>1 %R %s #%d %s %s - Last of %d (skipped) Identical messages",
						RptPRI, RptUTC, nameSTA, McuID, ProcID, MsgID, RptCNT) ;
				xSyslogSendMessage(SyslogBuffer, xLen) ;
			}
			// rebuild the NEW console message
			xLen = snprintfx(SyslogBuffer, syslogBUFSIZE, "%s %s ", ProcID, MsgID) ;
			vsnprintfx(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;
			RptCNT = 0 ;								// and reset the counter
		}

		// show the new message to the console...
		PRINT("%C%!R: #%d %s%C\n", xpfSGR(attrRESET, SyslogColors[MsgPRI & 0x07],0,0), MsgRUN, McuID, SyslogBuffer, attrRESET) ;
		// filter out reasons why message should not go to syslog host, then build and send
		if (FRflag && bSyslogCheckStatus(MsgPRI)) {
			xLen =	snprintfx(SyslogBuffer, syslogBUFSIZE, "<%u>1 %R %s #%d %s %s - ", MsgPRI, MsgUTC, nameSTA, McuID, ProcID, MsgID) ;
			xLen += vsnprintfx(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;
			xLen = xSyslogSendMessage(SyslogBuffer, xLen) ;

		} else {
			xLen = 0 ;
		}
	}
	xRtosSemaphoreGive(&SyslogMutex) ;
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
int32_t	IRAM_ATTR xSyslog(uint32_t Priority, const char * MsgID, const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
	int32_t iRV = xvSyslog(Priority, MsgID, format, vaList) ;
    va_end(vaList) ;
    return iRV ;
}

/**
 * xvLog() and xLog() - perform printfx() like output to a buffer and then to the console
 * @param	format
 * @param	vArgs or Args
 * @return	number of characters written to the buffer and then console
 */
int32_t	xvLog(const char * format, va_list vArgs) {
	IF_myASSERT(debugPARAM, INRANGE_MEM(format)) ;
	xRtosSemaphoreTake(&SyslogMutex, portMAX_DELAY) ;
	int32_t iRV = PRINT(format, vArgs) ;
	sSyslogCtx.maxTx = (iRV > sSyslogCtx.maxTx) ? iRV : sSyslogCtx.maxTx ;
	xRtosSemaphoreGive(&SyslogMutex) ;
	return iRV ;
}

int32_t	xLog(const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
	int32_t iRV = xvLog(format, vaList) ;
    va_end(vaList) ;
    return iRV ;
}

int32_t	xLogFunc(int32_t (*F)(char *, size_t)) {
	IF_myASSERT(debugPARAM, INRANGE_FLASH(F)) ;
	xRtosSemaphoreTake(&SyslogMutex, portMAX_DELAY) ;
	int32_t iRV = F(SyslogBuffer, syslogBUFSIZE) ;
	if (iRV > 0) {
		sSyslogCtx.maxTx = (iRV > sSyslogCtx.maxTx) ? iRV : sSyslogCtx.maxTx ;
	}
	xRtosSemaphoreGive(&SyslogMutex) ;
    return iRV ;
}

/**
 * vSyslogReport() - report x[v]Syslog() related information
 */
void	vSyslogReport(void) {
	if (bRtosCheckStatus(flagNET_SYSLOG)) {
		xNetReport(&sSyslogCtx, "SLOG", 0, 0, 0) ;
	}
	PRINT("\tmaxTX=%u  CurRpt=%d\n", sSyslogCtx.maxTx, RptCNT) ;
}

// #################################### Test and benchmark routines ################################

#if 0
#include	"crc.h"										// private component

void	vSyslogBenchmark(void) {
	char Test1[] = "SNTP vSntpTask ntp1.meraka.csir.co.za  2019-03-05T10:56:58.901Z  tOFF=78,873,521uS  tRTD=11,976uS" ;
	char Test2[] = "01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz 01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" ;

	uint32_t	crc1, crc2, crc3, crc4, crc5, crc6 ;
	vSysTimerReset(1 << systimerSLOG, systimerCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(systimerSLOG) ;
	crc1 = F_CRC_CalculaCheckSum((uint8_t *) Test1, sizeof(Test1)-1) ;
	crc4 = F_CRC_CalculaCheckSum((uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(systimerSLOG) ;
	vSysTimerShow(1 << systimerSLOG) ;

	vSysTimerReset(1 << systimerSLOG, systimerCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(systimerSLOG) ;
	crc2 = crc32_le(0, (uint8_t *) Test1, sizeof(Test1)-1) ;
	crc5 = crc32_le(0, (uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(systimerSLOG) ;
	vSysTimerShow(1 << systimerSLOG) ;

	vSysTimerReset(1 << systimerSLOG, systimerCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(systimerSLOG) ;
	crc3 = crcSlow((uint8_t *) Test1, sizeof(Test1)-1) ;
	crc6 = crcSlow((uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(systimerSLOG) ;
	vSysTimerShow(1 << systimerSLOG) ;

	PRINT("CRC #1=%u  #2=%u  #3=%u\n", crc1, crc2, crc3) ;
	PRINT("CRC #4=%u  #5=%u  #6=%u\n", crc4, crc5, crc6) ;
}
#endif
