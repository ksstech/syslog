/*
 * syslog.c
 * Copyright 2014-21 Andre M Maree / KSS Technologies (Pty) Ltd.
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
 *	#2	Only messages with SEVerity <= ioSLOGhi will be logged to the syslog server
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
#include	"hal_variables.h"
#include	"printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include	"socketsX.h"

#include	"FreeRTOS_Support.h"

#include	"x_errors_events.h"
#include	"x_stdio.h"
#include	"x_time.h"

#include	"hal_network.h"

#ifdef ESP_PLATFORM
	#include	"esp_log.h"
	#include	"esp32/rom/crc.h"					// ESP32 ROM routine
#else
	#include	"crc-barr.h"						// Barr group CRC
#endif

#include	<errno.h>
#include	<string.h>

#define	debugFLAG					0xF000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ##############################


// ###################################### Global variables #########################################

// '<7>1 2021/10/21T12:34.567: cc50e38819ec_WROVERv4_5C9 #0 esp_timer halVARS_ReportFlags - '
#define	SL_SIZEBUF1				120
#define	SL_SIZEBUF2				880
#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
	#define SL_CORES 				2
#else
	#define SL_CORES 				1
#endif

typedef union __attribute__((packed)) {
	struct {
		char buf0[SL_SIZEBUF1 + SL_SIZEBUF2];
		uint16_t len0, pad0;
	};
	struct {
		char buf1[SL_SIZEBUF1];
		char buf2[SL_SIZEBUF2];
		uint16_t len1, len2;
	};
} syslog_t;
syslog_t sSyslog[SL_CORES];

static SemaphoreHandle_t SyslogMutex ;
static netx_t sSyslogCtx ;

static uint64_t	* ptRunTime, * ptUTCTime ;
static uint32_t RptCRC = 0, RptCNT = 0;
static uint64_t RptRUN = 0, RptUTC = 0;
static uint8_t RptPRI = 0;
static char SyslogColors[8] = {
// 0 = Emergency	1 = Alert	2 = Critical	3 = Error
	colourFG_RED, colourFG_RED, colourFG_RED, colourFG_RED,
// 4 = Warning		5 = Notice		6 = Info		7 = Debug
	colourFG_YELLOW, colourFG_GREEN, colourFG_MAGENTA, colourFG_CYAN,
} ;

/* In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
 * could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
 * seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
 * level should be set to NOTICE. */

// ###################################### Public functions #########################################

/**
 * xSyslogInit() - Initialise the SysLog module
 * \param[in]	host name to log to
 * \param[in]	pointer to uSec runtime counter
 * \param[in]	pointer to uSec UTC time value
 * \return		1 if connection successful
 */
int	IRAM_ATTR xSyslogInit(const char * pcHostName, uint64_t * pRunTime, uint64_t * pUTCTime) {
	IF_myASSERT(debugPARAM, pcHostName && halCONFIG_inSRAM(pRunTime) && halCONFIG_inSRAM(pUTCTime)) ;
	if (sSyslogCtx.pHost != NULL || sSyslogCtx.sd > 0)
		vSyslogDisConnect() ;
	memset(&sSyslogCtx, 0, sizeof(sSyslogCtx)) ;
	ptRunTime = pRunTime ;
	ptUTCTime = pUTCTime ;
	sSyslogCtx.pHost = pcHostName ;
	return xSyslogConnect() ;
}

/**
 * vSyslogConnect() - establish connection to the selected syslog host
 * \return		1 if successful else 0
 */
int	IRAM_ATTR xSyslogConnect(void) {
	IF_myASSERT(debugPARAM, sSyslogCtx.pHost) ;
	if (bRtosCheckStatus(flagLX_STA) == 0)
		return 0 ;
	sSyslogCtx.sa_in.sin_family = AF_INET ;
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_UDP) ;
	sSyslogCtx.type				= SOCK_DGRAM ;
	sSyslogCtx.d_flags			= 0 ;
	sSyslogCtx.d_ndebug			= 1 ;					// disable debug in socketsX.c
	int	iRV = xNetOpen(&sSyslogCtx) ;
	if (iRV > erFAILURE) {
		iRV = xNetSetNonBlocking(&sSyslogCtx, flagXNET_NONBLOCK) ;
		if (iRV > erFAILURE) {
			xRtosSetStatus(flagNET_SYSLOG) ;
			return 1 ;
		}
	}
	xNetClose(&sSyslogCtx) ;
	return 0 ;
}

/**
 * vSyslogDisConnect()	De-initialise the SysLog module
 */
void IRAM_ATTR vSyslogDisConnect(void) {
	xRtosClearStatus(flagNET_SYSLOG) ;
	close(sSyslogCtx.sd) ;
	sSyslogCtx.sd = -1 ;
	IF_PRINT(debugTRACK && ioB1GET(ioRstrt), "disconnect\n") ;
}

bool IRAM_ATTR bSyslogCheckStatus(uint8_t MsgPRI) {
	if (bRtosCheckStatus(flagLX_STA) == 0) return 0;
	if (bRtosCheckStatus(flagNET_SYSLOG) == 0)
		return xSyslogConnect();
	return 1;
}

int	IRAM_ATTR xSyslogSendMessage(char * pcBuffer, int xLen) {
	// write directly to socket, not via xNetWrite(), to avoid recursing
	int	iRV = sendto(sSyslogCtx.sd, pcBuffer, xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in)) ;
	if (iRV == xLen) sSyslogCtx.maxTx = (xLen > sSyslogCtx.maxTx) ? xLen : sSyslogCtx.maxTx ;
	else vSyslogDisConnect() ;
	return iRV ;
}

/**
 * xvSyslog writes an RFC formatted message to syslog host
 * \brief		write to stdout & syslog host (if up and running)
 * \param[in]	Priority and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal printf()
 * \return		number of characters sent to server
 */
void IRAM_ATTR xvSyslog(int Level, const char * MsgID, const char * format, va_list vArgs) {
	// ANY message above this option value WILL be ignored....
	if ((Level % 8) > ioB3GET(ioSLOGhi))
		return;

	// Fix up incorrectly formatted messages
	MsgID = (MsgID == NULL) ? "null" : (*MsgID == 0) ? "empty" : MsgID;
	format = (format == NULL) ? "null" : (*format == 0) ? "empty" : format;

	// Handle state of scheduler and obtain the task name
	bool FRflag;
	char *	ProcID;
	int xLen = 0;
	xRtosSemaphoreTake(&SyslogMutex, portMAX_DELAY) ;
	if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING) {
		FRflag = 1;
		ProcID = pcTaskGetName(NULL) ;					// FreeRTOS v9.0.0 onwards uses short form function name
		IF_myASSERT(debugPARAM, halCONFIG_inSRAM(ProcID)) ;
		char * pcTmp  = ProcID ;
		while (*pcTmp) {
			if (*pcTmp == ' ')
				*pcTmp = '_' ;
			++pcTmp ;
		}
	} else {
		FRflag = 0 ;
		ProcID = (char *) "preX" ;
	}

	// Setup time, priority and related variables
	uint8_t MsgPRI = Level % 256 ;
	uint64_t MsgRUN, MsgUTC ;
	if (ptRunTime == NULL) {
		MsgRUN = MsgUTC = esp_log_timestamp() * MICROS_IN_MILLISEC ;
	} else {
		MsgRUN = *ptRunTime ;
		MsgUTC = *ptUTCTime ;
	}
	int McuID = 0 ;							// default in case not ESP32 or scheduler not running
#if defined(ESP_PLATFORM)
	#if !defined(CONFIG_FREERTOS_UNICORE)
	McuID = xPortGetCoreID();
	#endif
#endif

	// Step 2: build the console formatted message into the buffer
	xLen = snprintfx(SyslogBuffer, syslogBUFSIZE, "%s %s ", ProcID, MsgID) ;
	xLen += vsnprintfx(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;

	// Calc CRC to check for repeat message, handle accordingly
	uint32_t MsgCRC = crc32_le(0, (uint8_t *) SyslogBuffer, xLen) ;
#if defined(ESP_PLATFORM)								// use ROM based CRC lookup table
#else													// use fastest of external libraries
	uint32_t MsgCRC = crcSlow((uint8_t *) SyslogBuffer, xLen) ;
#endif

	if (MsgCRC == RptCRC && MsgPRI == RptPRI) {			// CRC & PRI same as previous message ?
		++RptCNT ;										// Yes, increment the repeat counter
		RptRUN = MsgRUN ;								// save timestamps of latest repeat
		RptUTC = MsgUTC ;
		xLen = 0 ;										// nothing was sent via network
	} else {											// new/different message
		RptCRC = MsgCRC ;
		RptPRI = MsgPRI ;
		if (RptCNT > 0) {								// if we have skipped messages
			printfx("%C%!.3R: #%d %s (x %d)%C\n", xpfSGR(SyslogColors[MsgPRI & 0x07], 0, 0, 0),
					MsgRUN, McuID, SyslogBuffer, RptCNT, 0) ;
			// build & send skipped message to host
			if (FRflag && bSyslogCheckStatus(MsgPRI)) {
				xLen =	snprintfx(SyslogBuffer, syslogBUFSIZE, "<%u>1 %.R %s #%d %s %s - Last of %d (skipped) Identical messages", RptPRI, RptUTC, nameSTA, McuID, ProcID, MsgID, RptCNT) ;
				xSyslogSendMessage(SyslogBuffer, xLen) ;
				// rebuild the NEW console message
				xLen = snprintf(SyslogBuffer, syslogBUFSIZE, "%s %s ", ProcID, MsgID) ;
				vsnprintf(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;
			}
			RptCNT = 0 ;								// and reset the counter
		}

		// show the new message to the console...
		printfx("%C%!.3R: #%d %s%C\n", xpfSGR(SyslogColors[MsgPRI & 0x07], 0, 0, 0), MsgRUN, McuID, SyslogBuffer, 0) ;
		// filter out reasons why message should not go to syslog host, then build and send
		if (FRflag && bSyslogCheckStatus(MsgPRI)) {
			xLen =	snprintfx(SyslogBuffer, syslogBUFSIZE, "<%u>1 %.R %s #%d %s %s - ",
					MsgPRI, MsgUTC, nameSTA, McuID, ProcID, MsgID) ;
			xLen += vsnprintf(&SyslogBuffer[xLen], syslogBUFSIZE - xLen, format, vArgs) ;
			xLen = xSyslogSendMessage(SyslogBuffer, xLen) ;
		} else {
			xLen = 0 ;
		}
	}
exit:
	xRtosSemaphoreGive(&SyslogMutex) ;
	return;
}

/**
 * Writes an RFC formatted message to syslog host
 * \brief		if syslog not up and running, write to stdout
 * \brief		avoid using pvRtosMalloc() or similar since also called from error/crash handlers
 * \param[in]	Priority, ProcID and MsgID as defined by RFC
 * \param[in]	format string and parameters as per normal printf()
 * \param[out]	none
 * \return		number of characters displayed(if only to console) or send(if to server)
 */
void IRAM_ATTR vSyslog(int Level, const char * MsgID, const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
	xvSyslog(Level, MsgID, format, vaList) ;
    va_end(vaList) ;
}

/**
 * vSyslogReport() - report x[v]Syslog() related information
 */
void vSyslogReport(void) {
	if (bRtosCheckStatus(flagNET_SYSLOG) == 1) {
		xNetReport(&sSyslogCtx, "SLOG", 0, 0, 0) ;
		printfx("\tmaxTX=%u  CurRpt=%d\n", sSyslogCtx.maxTx, RptCNT) ;
	}
}

// #################################### Test and benchmark routines ################################

#if 0
#include	"crc.h"										// private component

void	vSyslogBenchmark(void) {
	char Test1[] = "SNTP vSntpTask ntp1.meraka.csir.co.za  2019-03-05T10:56:58.901Z  tOFF=78,873,521uS  tRTD=11,976uS" ;
	char Test2[] = "01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz 01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" ;

	uint32_t	crc1, crc2, crc3, crc4, crc5, crc6 ;
	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc1 = F_CRC_CalculaCheckSum((uint8_t *) Test1, sizeof(Test1)-1) ;
	crc4 = F_CRC_CalculaCheckSum((uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc2 = crc32_le(0, (uint8_t *) Test1, sizeof(Test1)-1) ;
	crc5 = crc32_le(0, (uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc3 = crcSlow((uint8_t *) Test1, sizeof(Test1)-1) ;
	crc6 = crcSlow((uint8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	printfx("CRC #1=%u  #2=%u  #3=%u\n", crc1, crc2, crc3) ;
	printfx("CRC #4=%u  #5=%u  #6=%u\n", crc4, crc5, crc6) ;
}
#endif
