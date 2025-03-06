// syslog.c - Copyright (c) 2014-25 Andre M. Maree / KSS Technologies (Pty) Ltd.

/***************************************************************************************************
 * Theory of operation.
 *
 *	#1	Messages with SEVerity <= ioSLOGhi are sent to the console
 *	#2	Messages with SEVerity <= ioSLhost will be logged to the syslog server
 *
 *	To minimise the impact on application size the SL_xxxx macros must be used to in/exclude levels of info.
 * 		SL_DBG() to control inclusion and display of DEBUG type information
 *		SL_INFO() to control the next level of information verbosity
 *		SL_NOT() to control information inclusion/display of important events, NOT errors
 *		SL_WARN() to inform on concerns such as values closely approaching threshold
 *		SL_ERR() for errors that the system can/will recover from automatically
 *		SL_CRIT/ALRT/EMER() reserved for unrecoverable errors that should result in a system restart
*/

#include "hal_platform.h"
#include "certificates.h"
#include "hal_flash.h"
#include "hal_network.h"
#include "hal_options.h"
#include "hal_stdio.h"
#include "hal_timer.h"
#include "printfx.h"
#include "socketsX.h"
#include "syslog.h"
#include "errors_events.h"

#include <errno.h>

#ifdef ESP_PLATFORM
	#include "esp_log.h"
#endif

// ####################################### Macros ##################################################

#define debugFLAG 0xF000
#define debugTIMING (debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define debugTRACK (debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define debugPARAM (debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define debugRESULT (debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ###################################### BUILD : CONFIG definitions ##############################

#ifdef CONFIG_FREERTOS_UNICORE
	#define slCORES					1
#else
	#define slCORES					2
#endif

#define formatRFC5424 DRAM_STR("<%u>1 %.3Z %s %d %s - - %s ")
#define formatCONSOLE DRAM_STR("%C%!.3R %d %s %s ")
#define formatREPEATED DRAM_STR("Repeated %dx")
#define formatTERMINATE DRAM_STR("%C" strNL)

// '<7>1 2021/10/21T12:34.567: cc50e38819ec_WROVERv4_5C9 #0 esp_timer halVARS_Report????? - '
#define slSIZEBUF					512
#define slFILESIZE					10204				// MAX history (at boot) size before truncation
#define slFILENAME					"/syslog.txt"		// default file name in root directory
#define UNKNOWNMACAD				"#UnknownMAC#"		// MAC address marker in pre-wifi messages
#define slMS_LOCK_WAIT				1000

// ######################################### Structures ############################################

// ####################################### Local variables #########################################

static const char SyslogColors[8] = {
	colourFG_RED,					// Emergency
	colourFG_RED,					// Alert
	colourFG_RED,					// Critical
	colourFG_RED,					// Error
	colourFG_YELLOW,				// Warning
	colourFG_GREEN,					// Notice
	colourFG_MAGENTA,				// Info
	colourFG_CYAN,					// Debug
};
static netx_t sCtx = {0};
static u32_t RptCRC = 0, RptCNT = 0;
static u64_t RptRUN = 0, RptUTC = 0;
static u8_t RptPRI = 0;
static const char *RptTask = NULL, *RptFunc = NULL;
static report_t sRpt = { .Size = repSIZE_SET(0,0,0,1,sgrANSI,0,0) };
#if (appLITTLEFS == 1)
	static bool FileBuffer = 0;
#endif

#if (appOPTIONS == 0)
	static u8_t hostLevel = SL_LEV_HOST;
	static u8_t consoleLevel = SL_LEV_CONSOLE;
#endif

// ###################################### Global variables #########################################

SemaphoreHandle_t slNetMux = 0, slVarMux = 0;

// ##################################### Private functions #########################################

static int xSyslogRemoveTerminators(char * pBuf, int xLen) {
	while  (isspace((int) pBuf[xLen - 1]) != 0)
		pBuf[--xLen] = CHR_NUL;							// remove terminating white space character(s)
	return xLen;
}

/**
 * @brief	establish connection to the selected syslog host
 * @return	1 if successful else 0
 * @note	can only return 1 if scheduler running & L3 connected, 
*/
static bool IRAM_ATTR xSyslogConnect(void) {
	if ((xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) || halEventCheckStatus(flagLX_STA) == 0)
		return 0;
	if (sCtx.sd > 0) 									// already connected ?
		return 1;										// yes, exit with status OK
	sCtx.type = SOCK_DGRAM;
	sCtx.flags = SO_REUSEADDR;
	sCtx.sa_in.sin_family = AF_INET;
	sCtx.bSyslog = 1;									// mark as syslog port, so as not to recurse in xNet??????
	#if (appOPTIONS > 0)
		int Idx = xOptionGet(ioHostSLOG);				// if WL connected, NVS vars must be initialized (in stage 2.0/1)
		sCtx.pHost = HostInfo[Idx].pName;
		sCtx.sa_in.sin_port = htons(HostInfo[Idx].Port ? HostInfo[Idx].Port : IP_PORT_SYSLOG_UDP);
	#else
		sCtx.pHost = appDEFAULT_SL_HOST;				// options not part of application ?
		sCtx.sa_in.sin_port = htons(appDEFAULT_SL_PORT);// get from app_config...
	#endif
	// successfully opened && Receive TO set ok?
	if ((xNetOpen(&sCtx) > erFAILURE) && 				// successfully opened ?
		(xNetSetRecvTO(&sCtx, flagXNET_NONBLOCK) > erFAILURE)) {	// and RX timeout set ?
		return 1;										// yes, return all OK
	}
	xNetClose(&sCtx);									// no, trying closing
	return 0;											// and return status accordingly
}

/**
 * @brief
 */
static void vSyslogFileSend(void) {
	xRtosSemaphoreTake(&LFSmux, portMAX_DELAY);
	FILE *fp = fopen(slFILENAME, "r");
	if (fp == NULL)										// successfully opened file?			
		goto exit0;										// no failed
	if (fseek(fp, 0L, SEEK_END) != 0 || ftell(fp) == 0L)// Seek to end OK & something there?
		goto exit1;										// nope, something wrong
	rewind(fp);
	char * pBuf = malloc(slSIZEBUF);
	while (1) {
		char *pRV = fgets(pBuf, slSIZEBUF, fp);		// read string/line from file
		if (pRV != pBuf)								// nothing read or error?
			break;										// exit
		char * pTmp = strstr(pRV, UNKNOWNMACAD);		// Check if early message ie no MAC address
		if (pTmp != NULL)								// if UNKNOWNMACAD marker is present
			memcpy(pTmp, idSTA, lenMAC_ADDRESS*2);		// replace with actual MAC/hostname
		int xLen = strlen(pBuf);
		if (pBuf[xLen - 1] == CHR_LF)
			pBuf[--xLen] = CHR_NUL;						// remove terminating [CR]LF
		if (xNetSend(&sCtx, (u8_t *)pBuf, xLen) < 0)	// send message to host
			break;										// if error, abort sending
		vTaskDelay(pdMS_TO_TICKS(10));					// ensure WDT gets fed....
	}
	free(pBuf);
	FileBuffer = 0;
exit1:
	fclose(fp);
	unlink(slFILENAME);
exit0:
	xRtosSemaphoreGive(&LFSmux);
}

static void IRAM_ATTR xvSyslogSendMessage(int MsgPRI, tsz_t *psUTC, int McuID,
	const char *ProcID, const char *MsgID, char *pBuf, const char *format, va_list vaList) {
	int iRV, xLen;
	if (pBuf == NULL) {
		BaseType_t btSR = xRtosSemaphoreTake(&shUARTmux, portMAX_DELAY);
		wprintfx(&sRpt, formatCONSOLE, xpfCOL(SyslogColors[MsgPRI & 0x07],0), halTIMER_ReadRunTime(), McuID, ProcID, MsgID);
		wvprintfx(&sRpt, format, vaList);
		wprintfx(&sRpt, formatTERMINATE, xpfCOL(attrRESET,0));
		if (btSR == pdTRUE)
			xRtosSemaphoreGive(&shUARTmux);
	} else {
		if (idSTA[0] == 0)
			strcpy((char*)idSTA, UNKNOWNMACAD);			// very early message, WIFI not initialized
		xLen = snprintfx(pBuf, slSIZEBUF, formatRFC5424, MsgPRI, psUTC, idSTA, McuID, ProcID, MsgID);
		xLen += vsnprintfx(pBuf + xLen, slSIZEBUF - xLen - 1, format, vaList); // leave space for LF

		if (xSyslogConnect()) {							// Scheduler running, LxSTA up and connected
			#if (appLITTLEFS == 1)
				if (FileBuffer)
					vSyslogFileSend();
			#endif
			xLen = xSyslogRemoveTerminators(pBuf, xLen);
			if (xRtosSemaphoreTake(&slNetMux, pdMS_TO_TICKS(slMS_LOCK_WAIT)) == pdTRUE) {
				iRV = xNetSend(&sCtx, (u8_t *)pBuf, xLen);
				xRtosSemaphoreGive(&slNetMux);
				if (iRV == erFAILURE)
					xNetClose(&sCtx);
				else
					sCtx.maxTx = (iRV > sCtx.maxTx) ? iRV : sCtx.maxTx;
			}
		} else {
		#if (appLITTLEFS > 0)
			if (halEventCheckDevice(devMASK_LFS)) { 	// scheduler not (yet) running or LXSTA down, append to file...
				if (pBuf[xLen-1] != CHR_LF) {
					pBuf[xLen++] = CHR_LF;				// append LF if required
					pBuf[xLen] = CHR_NUL;				// and terminate
				}
				halFlashFileWrite(slFILENAME, "a", pBuf);
				FileBuffer = 1;
			}
		#endif
		}
	}
}

static void IRAM_ATTR xSyslogSendMessage(int MsgPRI, tsz_t *psUTC, int McuID, const char *ProcID,
										 const char *MsgID, char *pBuf, const char *format, ...) {
	va_list vaList;
	va_start(vaList, format);
	xvSyslogSendMessage(MsgPRI, psUTC, McuID, ProcID, MsgID, pBuf, format, vaList);
	va_end(vaList);
}

// ###################################### Public functions #########################################

// In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
// could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
// seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
// level should be set to NOTICE.

int xSyslogGetConsoleLevel(void) {
#if (appOPTIONS == 1)
	int iRV = xOptionGet(ioSLOGhi);
	return iRV ? iRV : SL_LEV_CONSOLE;
#else
	return consoleLevel;
#endif
}

int xSyslogGetHostLevel(void) {
#if (appOPTIONS == 1)
	int iRV = xOptionGet(ioSLhost);
	return iRV ? iRV : SL_LEV_HOST;
#else
	return hostLevel;
#endif
}

void vSyslogSetConsoleLevel(int Level) {
	if (Level <= SL_LEVEL_MAX) {
		#if (appOPTIONS == 1)
		vOptionSet(ioSLOGhi, Level);
		#else
		consoleLevel = Level;
		#endif
	}
}

void vSyslogSetHostLevel(int Level) {
	if (Level <= SL_LEVEL_MAX) {
	#if (appOPTIONS == 1)
		vOptionSet(ioSLhost, Level);
	#else
		hostLevel = Level;
	#endif
	}
}

void vSyslogFileCheckSize(void) {
	ssize_t Size = halFlashFileGetSize(slFILENAME);
	if (Size > slFILESIZE) {
		unlink(slFILENAME);			// file size > "slFILESIZE" in size....
		Size = 0;
	}
	FileBuffer = (Size > 0) ? 1 : 0;
}

/**
 * @brief		writes an RFC formatted message to syslog host
 * @brief		write to stdout & syslog host (if up and running)
 * @param[in]	Priority and MsgID as defined by RFC
 * @param[in]	format string and parameters as per normal printf()
 * @return		number of characters sent to server
*/
void IRAM_ATTR xvSyslog(int MsgPRI, const char *MsgID, const char *format, va_list vaList) {
	// discard all messages higher than console log level
	if ((MsgPRI & 0x07) > xSyslogGetConsoleLevel())
		return;	
	MsgID = (MsgID == NULL) ? "null" : (*MsgID == 0) ? "empty" : MsgID;

	// Handle state of scheduler and obtain the task name
	const char *ProcID = (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) ? DRAM_STR("preX") : pcTaskGetName(NULL);	
	u64_t CurRUN = halTIMER_ReadRunTime();
	if (sTSZ.usecs == 0)
		sTSZ.usecs = CurRUN;
	int McuID = esp_cpu_get_core_id();

	BaseType_t btSR = xRtosSemaphoreTake(&slVarMux, portMAX_DELAY);
	u32_t MsgCRC = 0;
	int xLen = crcprintfx(&MsgCRC, DRAM_STR("%s %s "), ProcID, MsgID);	// "Task Function "
	xLen += vcrcprintfx(&MsgCRC, format, vaList);						// "Task Function message parameters etc"

	if (MsgCRC == RptCRC && MsgPRI == RptPRI) {				// CRC & PRI same as previous message ?
		++RptCNT;											// Yes, increment the repeat counter
		RptRUN = CurRUN;									// save timestamps of latest repeat
		RptUTC = sTSZ.usecs;
		RptTask = ProcID;
		RptFunc = (char *)MsgID;
		if (btSR == pdTRUE)
			xRtosSemaphoreGive(&slVarMux);
	} else { // Different CRC and/or PRI
		// save trackers for immediate and future use...
		RptCRC = MsgCRC;
		u8_t TmpPRI = RptPRI;
		RptPRI = MsgPRI;
		u32_t TmpCNT = RptCNT;
		RptCNT = 0;
		u64_t TmpRUN = RptRUN;
		u64_t TmpUTC = RptUTC;
		const char *TmpTask = RptTask;
		const char *TmpFunc = RptFunc;
		if (btSR == pdTRUE)
			xRtosSemaphoreGive(&slVarMux);
		tsz_t TmpTSZ = {.pTZ = sTSZ.pTZ};

		// Handle console message(s)
		if (TmpCNT > 0) {
			TmpTSZ.usecs = TmpRUN;						// repeated message + count
			xSyslogSendMessage(TmpPRI, &TmpTSZ, McuID, TmpTask, TmpFunc, NULL, formatREPEATED, TmpCNT);
		}
		TmpTSZ.usecs = CurRUN;							// New message
		xvSyslogSendMessage(MsgPRI, &TmpTSZ, McuID, ProcID, MsgID, NULL, format, vaList);

		// Handle host message(s)
		if ((MsgPRI & 7) <= xSyslogGetHostLevel()) {	// filter based on higher priorities
			char *pBuf = malloc(slSIZEBUF);
			if (TmpCNT > 0) {
				TmpTSZ.usecs = TmpUTC;					// repeated message + count
				xSyslogSendMessage(TmpPRI, &TmpTSZ, McuID, TmpTask, TmpFunc, pBuf, formatREPEATED, TmpCNT);
			}
			xvSyslogSendMessage(MsgPRI, &sTSZ, McuID, ProcID, MsgID, pBuf, format, vaList);
			free(pBuf);
		}
	}
}

/**
 * Writes an RFC formatted message to syslog host
 * @brief		if syslog not up and running, write to stdout
 * @brief		avoid using malloc() or similar since also called from error/crash handlers
 * @param[in]	Priority, ProcID and MsgID as defined by RFC
 * @param[in]	format string and parameters as per normal printf()
 * @param[out]	none
 * @return		number of characters displayed(if only to console) or send(if to server)
*/
void IRAM_ATTR vSyslog(int MsgPRI, const char *MsgID, const char *format, ...) {
	va_list vaList;
	va_start(vaList, format);
	xvSyslog(MsgPRI, MsgID, format, vaList);
	va_end(vaList);
}

int IRAM_ATTR xSyslogError(const char *pcFN, int iRV) {
	vSyslog(SL_SEV_ERROR, pcFN, "iRV=%d (%s)", iRV, pcStrError(iRV));
	return (iRV > 0) ? -iRV : iRV;
}

/**
 * @brief	report syslog related information
*/
void vSyslogReport(report_t * psR) {
	if (sCtx.sd <= 0)
		return;
	if (psR == NULL) {
		psR = &sRpt;
		fmSET(aNL, 0);
	}
	xNetReport(psR, &sCtx, "SLOG", 0, 0, 0);
	wprintfx(psR, "\tmaxTX=%zu  CurRpt=%lu" strNL, sCtx.maxTx, RptCNT);
}

// #################################### Test and benchmark routines ################################

#if 0
#include "crc.h"

void vSyslogBenchmark(void) {
	char Test1[] = "SNTP vSntpTask ntp1.meraka.csir.co.za  2019-03-05T10:56:58.901Z  tOFF=78,873,521uS  tRTD=11,976uS" ;
	char Test2[] = "01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz 01234567890ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz" ;

	u32_t crc1, crc2, crc3, crc4, crc5, crc6 ;
	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc1 = F_CRC_CalculaCheckSum((u8_t *) Test1, sizeof(Test1)-1) ;
	crc4 = F_CRC_CalculaCheckSum((u8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc2 = crc32_le(0, (u8_t *) Test1, sizeof(Test1)-1) ;
	crc5 = crc32_le(0, (u8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	vSysTimerReset(1 << stSLOG, stCLOCKS, "SLOG", myUS_TO_CLOCKS(10), myUS_TO_CLOCKS(1000)) ;
	xSysTimerStart(stSLOG) ;
	crc3 = crcSlow((u8_t *) Test1, sizeof(Test1)-1) ;
	crc6 = crcSlow((u8_t *) Test2, sizeof(Test2)-1) ;
	xSysTimerStop(stSLOG) ;
	vSysTimerShow(1 << stSLOG) ;

	printfx("CRC #1=%u  #2=%u  #3=%u" strNL, crc1, crc2, crc3) ;
	printfx("CRC #4=%u  #5=%u  #6=%u" strNL, crc4, crc5, crc6) ;
}
#endif
