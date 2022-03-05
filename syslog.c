/*
 * syslog.c - Copyright 2014-22 Andre M Maree / KSS Technologies (Pty) Ltd.
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

#ifdef ESP_PLATFORM
	#include	"esp_log.h"
#endif

#include	"syslog.h"
#include	"hal_variables.h"
#include	"printfx.h"									// +x_definitions +stdarg +stdint +stdio
#include	"socketsX.h"
#include	"x_errors_events.h"
#include	"x_time.h"
#include	"hal_network.h"
#include	"hal_storage.h"

#define	debugFLAG					0xF000

#define	debugTIMING					(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define	debugTRACK					(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define	debugPARAM					(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define	debugRESULT					(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ##############################

// '<7>1 2021/10/21T12:34.567: cc50e38819ec_WROVERv4_5C9 #0 esp_timer halVARS_Report????? - '
#define	SL_SIZEBUF					512

#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
	#define SL_CORES 				2
#else
	#define SL_CORES 				1
#endif

// ######################################### Structures ############################################


// ###################################### Global variables #########################################

static SemaphoreHandle_t SL_NetMux = 0, SL_VarMux = 0;
static netx_t sCtx = { 0 };
static uint32_t RptCRC = 0, RptCNT = 0;
static uint64_t RptRUN = 0, RptUTC = 0;
static uint8_t RptPRI = 0;
static char SyslogColors[8] = {
// 0 = Emergency	1 = Alert	2 = Critical	3 = Error
	colourFG_RED, colourFG_RED, colourFG_RED, colourFG_RED,
//	4 = Warning			5 = Notice		6 = Info		7 = Debug
	colourFG_YELLOW, colourFG_GREEN, colourFG_MAGENTA, colourFG_CYAN,
};

// ###################################### Public functions #########################################

/* In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
 * could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
 * seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
 * level should be set to NOTICE. */

/**
 * @brief	establish connection to the selected syslog host
 * @return	1 if successful else 0
 */
static int IRAM_ATTR xSyslogConnect(void) {
	if (xRtosWaitStatusANY(flagLX_STA, pdMS_TO_TICKS(20)) != flagLX_STA)
		return 0;
	sCtx.pHost = HostInfo[ioB2GET(ioHostSLOG)].pName;
	IF_myASSERT(debugPARAM, sCtx.pHost) ;
	sCtx.sa_in.sin_family	= AF_INET ;
	sCtx.sa_in.sin_port		= htons(IP_PORT_SYSLOG_UDP) ;
	sCtx.type				= SOCK_DGRAM ;
	sCtx.d_flags			= 0 ;
	sCtx.d_ndebug			= 1 ;						// disable debug in socketsX.c
	int	iRV = xNetOpen(&sCtx) ;
	if (iRV > erFAILURE) {
		if (xNetSetNonBlocking(&sCtx, flagXNET_NONBLOCK) >= erSUCCESS) {
			#if	(halUSE_LITTLEFS == 1)
			// Check if buffered message file exists, if so send it....
			if (allSYSFLAGS(sfLFS)) {
				char * pBuf = pvRtosMalloc(SL_SIZEBUF);
				xRtosSemaphoreTake(&LFSmux, portMAX_DELAY);
				FILE * fp = fopen("syslog.txt", "r");
				if (fp != 0) {
					while (1) {
						char * pRV = fgets(pBuf, SL_SIZEBUF, fp);
						if (pRV != pBuf)
							break;
						int xLen = strlen(pBuf);
						if (pBuf[xLen-1] == CHR_LF)
							pBuf[--xLen] = CHR_NUL;							// remove terminating [CR]LF
						iRV = sendto(sCtx.sd, pBuf, xLen, sCtx.flags, &sCtx.sa, sizeof(sCtx.sa_in));
						vTaskDelay(pdMS_TO_TICKS(10));	// ensure WDT gets fed....
					}
				}
				iRV = fclose(fp);
				IF_myASSERT(debugRESULT, iRV == 0);
				unlink("syslog.txt");
				xRtosSemaphoreGive(&LFSmux);
			}
			#endif
			IF_RP(debugTRACK && ioB1GET(ioUpDown), "SLOG connect\n");
			return 1;
		}
	}
	xNetClose(&sCtx) ;
	return 0 ;
}

/**
 * @brief	de-initialise the SysLog module
 */
static void IRAM_ATTR vSyslogDisConnect(void) {
	close(sCtx.sd);
	sCtx.sd = -1;
	IF_RP(debugTRACK && ioB1GET(ioUpDown), "SLOG disconnect\n");
}

#define	formatRFC5424	DRAM_STR("<%u>1 %.Z %s #%d %s - - %s ")
#define formatCONSOLE 	DRAM_STR("%C%!.3R: #%d %s %s ")
#define formatREPEATED	DRAM_STR("Repeated %dx")
#define formatTERMINATE	DRAM_STR("%C\n")

static void IRAM_ATTR xvSyslogSendMessage(int PRI, tsz_t * psUTC, int McuID,
	char * ProcID, const char * MsgID, char * pBuf, const char * format, va_list vaList) {
	if (pBuf == NULL) {
		printfx_lock();
		printfx_nolock(formatCONSOLE, SyslogColors[PRI], psUTC->usecs, McuID, ProcID, MsgID);
		vprintfx_nolock(format, vaList);
		printfx_nolock(formatTERMINATE, attrRESET);
		printfx_unlock();
	} else {
		int xLen = snprintfx(pBuf, SL_SIZEBUF, formatRFC5424, PRI, psUTC, nameSTA, McuID, ProcID, MsgID);
		xLen += vsnprintfx(pBuf + xLen, SL_SIZEBUF - xLen - 1, format, vaList); // leave space for LF
		if (pBuf[xLen-1] != CHR_LF) {
			pBuf[xLen++] = CHR_LF;							// ensure terminating LF
			pBuf[xLen] = CHR_NUL;
		}
		if ((sCtx.sd > 0) || xSyslogConnect()) {			// LxSTA are up and connection established
			while (pBuf[xLen-1] == CHR_LF || pBuf[xLen-1] == CHR_CR) {
				pBuf[--xLen] = CHR_NUL;						// remove terminating CR/LF
			}
			xRtosSemaphoreTake(&SL_NetMux, portMAX_DELAY);
			int iRV = sendto(sCtx.sd, pBuf, xLen, sCtx.flags, &sCtx.sa, sizeof(sCtx.sa_in));
			xRtosSemaphoreGive(&SL_NetMux);
			if (iRV != erFAILURE) {
				sCtx.maxTx = (iRV > sCtx.maxTx) ? iRV : sCtx.maxTx ;
			} else {
				vSyslogDisConnect();
			}
		} else {
			#if	(halUSE_LITTLEFS == 1)
			if (allSYSFLAGS(sfLFS)) {	// L2+3 STA down, no connection, append to file...
				halFS_Write("syslog.txt", "a", pBuf);
			}
			#else
				// No file system (available or initialised) to write to
			#endif
		}

	}
}

static void IRAM_ATTR xSyslogSendMessage(int PRI, tsz_t * psUTC, int McuID,
	char * ProcID, const char * MsgID, char * pBuf, const char * format, ...) {
    va_list vaList;
    va_start(vaList, format);
    xvSyslogSendMessage(PRI, psUTC, McuID, ProcID, MsgID, pBuf, format, vaList);
    va_end(vaList);
}

/**
 * @brief		writes an RFC formatted message to syslog host
 * @brief		write to stdout & syslog host (if up and running)
 * @param[in]	Priority and MsgID as defined by RFC
 * @param[in]	format string and parameters as per normal printf()
 * @return		number of characters sent to server
 */
void IRAM_ATTR xvSyslog(int Level, const char * MsgID, const char * format, va_list vaList) {
	// Fix up incorrectly formatted messages
	MsgID = (MsgID == NULL) ? "null" : (*MsgID == 0) ? "empty" : MsgID;
	format = (format == NULL) ? "null" : (*format == 0) ? "empty" : format;

	// ANY message PRI/level above this option value WILL be ignored....
	uint8_t MsgPRI = Level % 8;
	if (MsgPRI > ioB3GET(ioSLOGhi))
		return;

	// Handle state of scheduler and obtain the task name
	char *	ProcID;
	if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
		ProcID = (char *) DRAM_STR("preX");
	} else {
		ProcID = pcTaskGetName(NULL);
		char * pcTmp  = ProcID;
		while (*pcTmp) {
			if (*pcTmp == ' ')
				*pcTmp = '_';
			++pcTmp;
		}
	}

	if (RunTime == 0ULL)
		RunTime = sTSZ.usecs = (uint64_t) esp_log_timestamp() * (uint64_t) MICROS_IN_MILLISEC;

	#if defined(ESP_PLATFORM) && !defined(CONFIG_FREERTOS_UNICORE)
	int McuID = cpu_hal_get_core_id();
	#else
	int McuID = 0;					// default in case not ESP32 or scheduler not running
	#endif

	uint32_t MsgCRC = 0;
	int xLen = crcprintfx(&MsgCRC, DRAM_STR("%s %s "), ProcID, MsgID);	// "Task Function "
	xLen += vcrcprintfx(&MsgCRC, format, vaList);				// "Task Function message parameters etc"

	xRtosSemaphoreTake(&SL_VarMux, portMAX_DELAY);
	if (MsgCRC == RptCRC && MsgPRI == RptPRI) {			// CRC & PRI same as previous message ?
		++RptCNT;										// Yes, increment the repeat counter
		RptRUN = RunTime;								// save timestamps of latest repeat
		RptUTC = sTSZ.usecs;
		xRtosSemaphoreGive(&SL_VarMux);
	} else {											// different message
		// TmpCNT, TmpPRI, TmpRUN & TmpUTC used to accurately distinguish the "repeated ??x" message
		uint8_t TmpCNT = RptCNT;
		uint8_t TmpPRI = RptPRI;
		// Save current MsgCRC & MsgPRI for determining future repeated message
		RptCRC = MsgCRC;
		RptPRI = MsgPRI;
		RptCNT = 0;										// and reset the counter
		// Start building & display/sending of message[s]
		tsz_t TmpUTC = {.pTZ = sTSZ.pTZ };
		xRtosSemaphoreGive(&SL_VarMux);
		// Handle console message(s)
		if (TmpCNT > 0) {								// previously skipped repeated messages
			TmpUTC.usecs = RptRUN;
			xSyslogSendMessage(TmpPRI, &TmpUTC, McuID, ProcID, MsgID, NULL, formatREPEATED, TmpCNT);
		}
		TmpUTC.usecs = RunTime;
		xvSyslogSendMessage(MsgPRI, &TmpUTC, McuID, ProcID, MsgID, NULL, format, vaList);

		// Handle host message(s)
		if (MsgPRI <= ioB3GET(ioSLhost)) {
			char * pBuf = pvRtosMalloc(SL_SIZEBUF);
			if (TmpCNT > 0) {							// previously skipped repeated messages ?
				TmpUTC.usecs = RptUTC;
				xSyslogSendMessage(TmpPRI, &TmpUTC, McuID, ProcID, MsgID, pBuf, formatREPEATED, TmpCNT);
			}
			TmpUTC.usecs = sTSZ.usecs;
			xvSyslogSendMessage(MsgPRI, &sTSZ, McuID, ProcID, MsgID, pBuf, format, vaList);
			vRtosFree(pBuf);
		}
	}
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
	if (sCtx.sd > 0) {
		xNetReport(&sCtx, "SLOG", 0, 0, 0) ;
		printfx("\tmaxTX=%u  CurRpt=%d\n", sCtx.maxTx, RptCNT) ;
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
