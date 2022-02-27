/*
 * syslog.c
 * Copyright 2014-22 Andre M Maree / KSS Technologies (Pty) Ltd.
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
 *		SL_CRIT/ALRT/EMER() reserved for unrecoverable errors that should result in a system restart
 */

#include	<errno.h>
#include	<string.h>

#include	"syslog.h"
#include	"hal_variables.h"
#include	"printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include	"socketsX.h"
#include	"FreeRTOS_Support.h"
#include	"x_errors_events.h"
#include	"x_time.h"
#include	"hal_network.h"

#ifdef ESP_PLATFORM
	#include	"esp_log.h"
	#include	"esp32/rom/crc.h"					// ESP32 ROM routine
#else
	#include	"crc-barr.h"						// Barr group CRC
#endif

#define	debugFLAG					0xF000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ##############################

// '<7>1 2021/10/21T12:34.567: cc50e38819ec_WROVERv4_5C9 #0 esp_timer halVARS_ReportFlags - '
#define	SL_SIZEBUF1				120
#define	SL_SIZEBUF2				880
#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
	#define SL_CORES 				2
#else
	#define SL_CORES 				1
#endif

// ###################################### Global variables #########################################

static char SyslogColors[8] = {
// 0 = Emergency	1 = Alert	2 = Critical	3 = Error
	colourFG_RED, colourFG_RED, colourFG_RED, colourFG_RED,
//	4 = Warning			5 = Notice		6 = Info		7 = Debug
	colourFG_YELLOW, colourFG_GREEN, colourFG_MAGENTA, colourFG_CYAN,
};

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

static uint32_t RptCRC = 0, RptCNT = 0;
static uint64_t RptRUN = 0, RptUTC = 0;
static uint8_t RptPRI = 0;

// ###################################### Public functions #########################################

/* In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
 * could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
 * seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
 * level should be set to NOTICE. */

/**
 * @brief	establish connection to the selected syslog host
 * @return	1 if successful else 0
 */
static int	IRAM_ATTR xSyslogConnect(void) {
	if (bRtosCheckStatus(flagLX_STA) == 0)
		return 0 ;
	sSyslogCtx.pHost = HostInfo[ioB2GET(ioHostSLOG)].pName;
	IF_myASSERT(debugPARAM, sSyslogCtx.pHost) ;
	sSyslogCtx.sa_in.sin_family = AF_INET ;
	sSyslogCtx.sa_in.sin_port   = htons(IP_PORT_SYSLOG_UDP) ;
	sSyslogCtx.type				= SOCK_DGRAM ;
	sSyslogCtx.d_flags			= 0 ;
	sSyslogCtx.d_ndebug			= 1 ;					// disable debug in socketsX.c
	int	iRV = xNetOpen(&sSyslogCtx) ;
	if (iRV > erFAILURE) {
		iRV = xNetSetNonBlocking(&sSyslogCtx, flagXNET_NONBLOCK) ;
		if (iRV >= erSUCCESS) {
			return 1;
		}
	}
	xNetClose(&sSyslogCtx) ;
	return 0 ;
}

/**
 * @brief	de-initialise the SysLog module
 */
static void IRAM_ATTR vSyslogDisConnect(void) {
	close(sSyslogCtx.sd);
	sSyslogCtx.sd = -1;
	IF_RP(debugTRACK && ioB1GET(ioUpDown), "SLOG disconnect\n");
}

static void IRAM_ATTR vvSyslogPrintMessage(int McuID, char * ProcID, const char * MsgID, const char * format, va_list vArgs) {
	int xLen = snprintfx(&sSyslog[McuID].buf2[0], SO_MEM(syslog_t, buf2), "%s %s - ", ProcID, MsgID);
	xLen += vsnprintfx(&sSyslog[McuID].buf2[xLen], SO_MEM(syslog_t, buf2) - xLen, format, vArgs);
	if (sSyslog[McuID].buf2[xLen-1] == CHR_LF) {
		sSyslog[McuID].buf2[--xLen] = CHR_NUL;			// remove terminating [CR]LF
	}
	sSyslog[McuID].len2 = xLen;
}

static void IRAM_ATTR vSyslogPrintMessage(int McuID, char * ProcID, const char * MsgID, const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
    vvSyslogPrintMessage(McuID, ProcID, MsgID, format, vaList) ;
    va_end(vaList) ;
}

static int IRAM_ATTR xSyslogSendMessage(int PRI, uint64_t UTC, int McuID) {
	int xLen = snprintfx(&sSyslog[McuID].buf0[0], SO_MEM(syslog_t, buf0),
			"<%u>1 %.R %s #%d %s", PRI, UTC, nameSTA, McuID, &sSyslog[McuID].buf2[0]);
	if ((sSyslogCtx.sd > 0) || xSyslogConnect()) {		// LxSTA are up and connection established
		iRV = sendto(sSyslogCtx.sd, &sSyslog[McuID].buf0[0], xLen, 0, &sSyslogCtx.sa, sizeof(sSyslogCtx.sa_in));
		if (iRV == xLen) {
			sSyslogCtx.maxTx = (xLen > sSyslogCtx.maxTx) ? xLen : sSyslogCtx.maxTx ;
		} else {
			vSyslogDisConnect();
		}
	}
	#if	(halUSE_LITTLEFS == 1)
	else if (allSYSFLAGS(sfLFS)) {	// LxSTA all down, no connection, append to file...
		FILE * fp = fopen("syslog.txt", "a");
		IF_myASSERT(debugRESULT, fp != 0);
		iRV = fwrite(sSyslog[McuID].buf0, 1, xLen+1, fp);
		fclose(fp);
		IF_myASSERT(debugRESULT, iRV == xLen+1);
	}
	#endif
	return iRV;
}

/**
 * @brief		writes an RFC formatted message to syslog host
 * @brief		write to stdout & syslog host (if up and running)
 * @param[in]	Priority and MsgID as defined by RFC
 * @param[in]	format string and parameters as per normal printf()
 * @return		number of characters sent to server
 */
void IRAM_ATTR xvSyslog(int Level, const char * MsgID, const char * format, va_list vArgs) {
	// ANY message PRI/level above this option value WILL be ignored....
	uint8_t MsgPRI = Level % 8;
//	if (allSYSFLAGS(sfAPPSTAGE) && (Level % 8) > ioB3GET(ioSLOGhi))
	if (MsgPRI > ioB3GET(ioSLOGhi))
		return;

	// Fix up incorrectly formatted messages
	MsgID = (MsgID == NULL) ? "null" : (*MsgID == 0) ? "empty" : MsgID;
	format = (format == NULL) ? "null" : (*format == 0) ? "empty" : format;

	// Handle state of scheduler and obtain the task name
	char *	ProcID;
	uint32_t MsgCRC;
	if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
		ProcID = (char *) "preX";
	} else {
		ProcID = pcTaskGetName(NULL);
		char * pcTmp  = ProcID;
		while (*pcTmp) {
			if (*pcTmp == ' ')
				*pcTmp = '_';
			++pcTmp;
		}
	}

	if (RunTime == 0ULL) {
		RunTime = sTSZ.usecs = (uint64_t) esp_log_timestamp() * (uint64_t) MICROS_IN_MILLISEC;
	}

	#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
	int McuID = cpu_hal_get_core_id();
	#else
	int McuID = 0;					// default in case not ESP32 or scheduler not running
	#endif

	// Build the console formatted message into the buffer (basis for CRC comparison)
	xRtosSemaphoreTake(&SyslogMutex, portMAX_DELAY);
	vvSyslogPrintMessage(McuID, ProcID, MsgID, format, vArgs);

	// Calc CRC to check for repeat message, handle accordingly
	#if defined(ESP_PLATFORM)								// use ROM based CRC lookup table
	MsgCRC = crc32_le(0, (uint8_t *) sSyslog[McuID].buf2, sSyslog[McuID].len2);
	#else													// use fastest of external libraries
	MsgCRC = crcSlow((uint8_t *) sSyslog[McuID].buf2, sSyslog[McuID].len2);
	#endif

	if (MsgCRC == RptCRC && MsgPRI == RptPRI) {			// CRC & PRI same as previous message ?
		++RptCNT;										// Yes, increment the repeat counter
		RptRUN = RunTime;								// save timestamps of latest repeat
		RptUTC = sTSZ.usecs;
	} else {											// different message
		if (RptCNT > 0) {								// previously skipped repeated messages ?
			printfx("%C%!.3R: #%d Repeated %dx%C\n", SyslogColors[RptPRI & 7], RptRUN, McuID, RptCNT, 0);
			if (FRflag && bSyslogCheckStatus(RptPRI)) {	// process skipped message to host
				vSyslogPrintMessage(McuID, ProcID, MsgID, "Repeated %dx", RptCNT);
				xSyslogSendMessage(RptPRI, RptUTC, McuID);
				vvSyslogPrintMessage(McuID, ProcID, MsgID, format, vArgs);	// rebuild console message
			}
			RptCNT = 0;					// and reset the counter
		}
		// process new message...
		RptCRC = MsgCRC;
		RptPRI = MsgPRI;
		printfx("%C%!.3R: #%d %s%C\n", SyslogColors[MsgPRI & 7], RunTime, McuID, &sSyslog[McuID].buf2[0], 0);
		if (FRflag && bSyslogCheckStatus(MsgPRI))
			xSyslogSendMessage(MsgPRI, sTSZ.usecs, McuID);
	}
	xRtosSemaphoreGive(&SyslogMutex);
}

/**
 * Writes an RFC formatted message to syslog host
 * @brief		if syslog not up and running, write to stdout
 * @brief		avoid using pvRtosMalloc() or similar since also called from error/crash handlers
 * @param[in]	Priority, ProcID and MsgID as defined by RFC
 * @param[in]	format string and parameters as per normal printf()
 * @param[out]	none
 * @return		number of characters displayed(if only to console) or send(if to server)
 */
void IRAM_ATTR vSyslog(int Level, const char * MsgID, const char * format, ...) {
    va_list vaList ;
    va_start(vaList, format) ;
	xvSyslog(Level, MsgID, format, vaList) ;
    va_end(vaList) ;
}

int IRAM_ATTR xSyslogError(const char * pcFN, int iRV) {
#ifdef ESP_PLATFORM
	vSyslog(SL_SEV_ERROR, pcFN, "iRV=0x%X (%s)", iRV, esp_err_to_name(iRV));
	return (iRV > 0) ? -iRV : iRV;
#else
	vSyslog(SL_SEV_ERROR, pcFN, "iRV=0x%X (%s)", iRV, strerr(iRV)) ;
	return iRV;
#endif
}

/**
 * @brief		report syslog related information
 */
void vSyslogReport(void) {
	if (sSyslogCtx.sd > 0) {
		xNetReport(&sSyslogCtx, "SLOG", 0, 0, 0) ;
		printfx("\tmaxTX=%u  CurRpt=%d\n", sSyslogCtx.maxTx, RptCNT) ;
	}
}

// #################################### Test and benchmark routines ################################

#if 0
#include	"crc.h"										// private component

void vSyslogBenchmark(void) {
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
