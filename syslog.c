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
#include "syslog.h"
#include "certificates.h"
#include "filesys.h"
#include "hal_network.h"
#include "hal_timer.h"
#include "hal_usart.h"
#include "socketsX.h"
#include "errors_events.h"

#include <errno.h>

#ifdef ESP_PLATFORM
	#include "esp_log.h"
#endif

// ####################################### Macros ##################################################

#define debugFLAG 				0xF000
#define debugTIMING				(debugFLAG_GLOBAL & debugFLAG & 0x1000)
#define debugTRACK				(debugFLAG_GLOBAL & debugFLAG & 0x2000)
#define debugPARAM				(debugFLAG_GLOBAL & debugFLAG & 0x4000)
#define debugRESULT				(debugFLAG_GLOBAL & debugFLAG & 0x8000)

// ######################################### Structures ############################################

typedef struct {
	struct __attribute__((packed)) {
		u16_t count:16;
		u8_t pri:8;
		u8_t core:4;
		u8_t spare:4;
	};
	u32_t crc;
	u64_t run, utc;
	const char *task, *func;
} sl_vars_t;

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
static netx_t sCtx = { 0 };
static sl_vars_t sRpt = { 0 };
#if (appLITTLEFS == 1)
	static bool FileBuffer = 0;
#endif

#if (appOPTIONS == 0)
	static u8_t hostLevel = SL_LEV_HOST;
	static u8_t consoleLevel = SL_LEV_CONSOLE;
#endif

char SLbuffer[2][slSIZEBUF] = { 0 };

// ###################################### Global variables #########################################

SemaphoreHandle_t shSLsock = 0, shSLvars = 0;

// ##################################### Private functions #########################################

/**
 * @brief	establish connection to the selected syslog host
 * @return	1 if successful else 0
 * @note	can only return 1 if scheduler running & L3 connected, 
*/
static bool IRAM_ATTR xSyslogConnect(void) {
	// step 1: If scheduler not running or L2+3 not ready, fail
	if ((xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) || halEventCheckStatus(flagLX_STA) == 0)
		return 0;

	// step 2: If unable to take the semaphore, fail
	if (xRtosSemaphoreTake(&shSLsock, slMS_LOCK_WAIT) == pdFALSE)
		return 0;

	// step 3: If already connected, return success
	int iRV = 1;
	if (sCtx.sd > 0)
		goto exit;

	// step 4: setup basic parameters for syslog connection
	#if (appOPTIONS > 0)
		int Idx = xOptionGet(ioHostSLOG);				// if WL connected, NVS vars must be initialized (in stage 2.0/1)
		sCtx.pHost = HostInfo[Idx].pName;
		sCtx.sa_in.sin_port = htons(HostInfo[Idx].Port ? HostInfo[Idx].Port : IP_PORT_SYSLOG_UDP);
	#else
		sCtx.pHost = appDEFAULT_SL_HOST;				// options not part of application ?
		sCtx.sa_in.sin_port = htons(appDEFAULT_SL_PORT);// get from app_config...
	#endif
	sCtx.flags = SO_REUSEADDR;
	sCtx.sa_in.sin_family = AF_INET;
	sCtx.c.type = SOCK_DGRAM;
	sCtx.c.NoSyslog = 1;								// mark as syslog port, so as not to recurse in xNetSyslog

	// step 5: before openng, close any zombie sockets
	xNetCloseDuplicates(sCtx.sa_in.sin_port);

	// step 6: open socket connection... AMM check if blocking really required!!!
	if ((xNetOpen(&sCtx) < erSUCCESS) || 				// open failed ?
		(xNetSetRecvTO(&sCtx, flagXNET_NONBLOCK) < erSUCCESS)) {	// RX timeout failed ?
		xNetClose(&sCtx);								// try closing
		iRV = 0;										// return failure...
	}
exit:
	xRtosSemaphoreGive(&shSLsock);
	return iRV;											// and return status accordingly
}

#define formatREPEATED		DRAM_STR("Repeated %dx")
#define formatCONSOLE1		DRAM_STR("%C%!.3R %d %s %s ")	// 	ANSI colour, UTC, core#, task, function
#define formatCONSOLE2		DRAM_STR("%C" strNL)
#define formatPAPERTRAIL	DRAM_STR("<%u>1 %.3R %s %s/%d %s - - ")		/* papertrailapp.com "main/0/Devices" */
#define formatRFC5424		DRAM_STR("<%d>1 %.3R %s %s %d %s - ")		/* RFC compliant "main 0 Devices" */

static int IRAM_ATTR xSyslogRemoveTerminators(char * pBuf, int xLen) {
	while  (isspace((int) pBuf[xLen - 1]) != 0)
		pBuf[--xLen] = CHR_NUL;							// remove terminating white space character(s)
	return xLen;
}

static void IRAM_ATTR xvSyslogConsole(sl_vars_t * psV, const char * format, va_list vaList) {
	report_t sRpt = {
		.pcAlloc = &SLbuffer[psV->core][0],
		.pcBuf = &SLbuffer[psV->core][0],
		.Size = repSIZE_SET(sBUFFER,sgrANSI,0,0,slSIZEBUF)
	 };
	int xLen = xReport(&sRpt, formatCONSOLE1, xpfCOL(SyslogColors[psV->pri&7],0), psV->run, psV->core, psV->task, psV->func);
	if (format)	xLen += xvReport(&sRpt, format, vaList);
	else		xLen += xReport(&sRpt, formatREPEATED, psV->count);
	xLen += xReport(&sRpt, formatCONSOLE2, xpfCOL(attrRESET,0));
	write(STDOUT_FILENO, sRpt.pcAlloc, xLen);			// low level unbuffered API
}

static void IRAM_ATTR xvSyslogHost(sl_vars_t * psV, const char * format, va_list vaList) {
	report_t sRpt = {
		.pcAlloc = &SLbuffer[psV->core][0],
		.pcBuf = &SLbuffer[psV->core][0],
		.Size = repSIZE_SET(sBUFFER,sgrNONE,0,0,slSIZEBUF)
	 };
	if (idSTA[0] == 0)									/* very early message, not WIFI yet */
		strcpy((char*)idSTA, UNKNOWNMACAD);				/* insert MAC address placemaker */
	int xLen = xReport(&sRpt, formatPAPERTRAIL, psV->pri, psV->utc, idSTA, psV->task, psV->core, psV->func);
	if (format)	xLen += xvReport(&sRpt, format, vaList);
	else		xLen += xReport(&sRpt, formatREPEATED, psV->count);

	// If check scheduler and LxSTA, take semaphore and if all ok, send the message
	int iRV = erFAILURE;
	if (xSyslogConnect() && xRtosSemaphoreTake(&shSLsock, pdMS_TO_TICKS(slMS_LOCK_WAIT)) == pdTRUE) {
		xLen = xSyslogRemoveTerminators(sRpt.pcAlloc, xLen);
		iRV = xNetSend(&sCtx, (u8_t *)sRpt.pcAlloc, xLen);
		if (iRV >= erSUCCESS) {							/* message successfully sent? */
			sCtx.maxTx = (iRV > sCtx.maxTx) ? iRV : sCtx.maxTx;	/* yes, update running stats */
		} else {										/* no, close the connection */
			xNetClose(&sCtx);							/* iRV already set for persisting */
		}
		xRtosSemaphoreGive(&shSLsock);
	}
	#if (appLITTLEFS > 0)		/* HOST not accessible try send to LFS if available ***********/
	if (iRV < erSUCCESS && halEventCheckDevice(devMASK_LFS)) {
		if (sRpt.pcAlloc[xLen-1] != CHR_LF) {			// yes, if last character not a LF
			sRpt.pcAlloc[xLen++] = CHR_LF;				// append LF for later fgets()
			sRpt.pcAlloc[xLen] = CHR_NUL;				// and terminate
		}
		xFileSysFileWrite(slFILENAME, "ax", sRpt.pcAlloc);	// open append exclusive
		FileBuffer = 1;
	}
	#endif
}

// ###################################### Public functions #########################################

int xSyslogCheckDuplicates(int sock, struct sockaddr_in * addr) {
	// Check for same port but sockets not same as current context
	if ((htons(addr->sin_port) == sCtx.sa_in.sin_port) && (sock != sCtx.sd)) {
		close(sock);
		return 1;
	}
	return 0;
}

int xSyslogGetConsoleLevel(void) {
#if (appOPTIONS > 0)
	int iRV = xOptionGet(ioSLOGhi);
	return iRV ? iRV : SL_LEV_CONSOLE;
#else
	return consoleLevel;
#endif
}

int xSyslogGetHostLevel(void) {
#if (appOPTIONS > 0)
	int iRV = xOptionGet(ioSLhost);
	return iRV ? iRV : SL_LEV_HOST;
#else
	return hostLevel;
#endif
}

void vSyslogSetConsoleLevel(int Level) {
	if (Level > SL_LEV_MAX)
		Level = SL_LEV_MAX;
#if (appOPTIONS > 0)
	vOptionSet(ioSLOGhi, Level);
#else
	consoleLevel = Level;
#endif
}

// In the case where the log level is set to DEBUG in ESP-IDF the volume of messages being generated
// could flood the IP stack and cause watchdog timeouts. Even if the timeout is changed from 5 to 10
// seconds the crash can still occur. In order to minimise load on the IP stack the minimum severity
// level should be set to NOTICE.

void vSyslogSetHostLevel(int Level) {
	if (Level > SL_LEV_MAX)
		Level = SL_LEV_MAX;
#if (appOPTIONS > 0)
	vOptionSet(ioSLhost, Level);
#else
	hostLevel = Level;
#endif
}

#if (appLITTLEFS == 1)
/**
 * @brief	Send contents (if any) of syslog offline buffer file to host
 */
void vSyslogFileSend(void) {
	// step 1: check if scheduler running, LxSTA up and connected
	if (xSyslogConnect() == 0)
		return;
	// step 2: check if anything there to send
	if (xFileSysGetFileSize(slFILENAME) <= 0)
		return;
	// step 3: protect the whole operation
	if (xRtosSemaphoreTake(&shSLsock, slMS_LOCK_WAIT) == pdFALSE)	/* semaphore taken? */
		return;														/* no, return for now */
	// step 4: try to lock file for read [and delete/unlink]
	if (xRtosSemaphoreTake(&shLFSmux, slMS_LOCK_WAIT) == pdFALSE)
		goto exit0;
	// step 5: try to open the file for read
	FILE *fp = fopen(slFILENAME, "r");
	if (fp == NULL)										/* successfully opened file? */
		goto exit1;										/* no, release both semaphores and return */

	// step 6: rewind and start sending
	int iRV = erSUCCESS;								// default to force file deletion at exit
	char * pBuf = malloc(slSIZEBUF);
	while (fgets(pBuf, slSIZEBUF, fp) != NULL) {
		// step 6a: fix placeholder MAC if required
		char * pTmp = strstr(pBuf, UNKNOWNMACAD);		// Check if early message ie no MAC address
		if (pTmp)										// if UNKNOWNMACAD marker is present
			memcpy(pTmp, idSTA, lenMAC_ADDRESS*2);		// replace with actual MAC/hostname

		// step 6b: trim extra terminators from the end
		int xLen = strlen(pBuf);
		xLen = xSyslogRemoveTerminators(pBuf, xLen);	// remove terminating [CR]LF
		if (xLen == 0)									// if nothing left to send (was just terminators...)
			break;

		// step 6c: send whatever remains of message (if any)
		iRV = xNetSend(&sCtx, (u8_t *)pBuf, xLen);		// send contents of buffer
		if (iRV <= 0) {									// message send failed?
			xNetClose(&sCtx);							// yes, close connection
			break;										// and abort sending
		}
		vTaskDelay(pdMS_TO_TICKS(slMS_FILESEND_DLY));	// ensure WDT gets fed....
	}
	free(pBuf);											// always free buffer

	// step 7: close the file and delete if successfully sent (add EOF and error checks to make sure?)
	fclose(fp);											// always close the file
	if (iRV >= erSUCCESS) {								// if last send was successful
		FileBuffer = 0;									// clear flag used to check for sending
		unlink(slFILENAME);								// delete the file
	}
exit1:
	xRtosSemaphoreGive(&shLFSmux);
exit0:
	xRtosSemaphoreGive(&shSLsock);
}
#endif

void vSyslogFileCheckSize(void) {
	ssize_t Size = xFileSysGetFileSize(slFILENAME);
	if (Size > slFILESIZE) {							// if file size > slFILESIZE
		unlink(slFILENAME);								// remove file
		Size = 0;										// discard content
	}
	FileBuffer = (Size > 0) ? 1 : 0;					// set flag if anything in file
}

void IRAM_ATTR xvSyslog(int MsgPRI, const char *FuncID, const char *format, va_list vaList) {
	// step 0: check if anything in file that needs sending, do so ASAP
	#if (appLITTLEFS == 1)
	if (FileBuffer)
		vSyslogFileSend();
	#endif

	// step 1: check if message priority outside console threshold
	if ((MsgPRI & 7) > xSyslogGetConsoleLevel())
		return;

	// step 2: handle state of scheduler and obtain the task name
	sl_vars_t sMsg;
	sMsg.pri = MsgPRI;
	sMsg.func = (FuncID == NULL) ? "null" : (*FuncID == 0) ? "empty" : FuncID;
	sMsg.count = 0;
	sMsg.core = esp_cpu_get_core_id();
	sMsg.run = halTIMER_ReadRunTime();
	sMsg.utc = sTSZ.usecs;
	sMsg.task = (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) ? DRAM_STR("preX") : pcTaskGetName(NULL);	

	// step 3: calculate CRC for current message 
	crcprintfx(&sMsg.crc, DRAM_STR("%s %s "), sMsg.task, sMsg.func);	// "Task Function "
	vcrcprintfx(&sMsg.crc, format, vaList);				//  add message parameters etc"

	// step 4: semaphore protect all local variables
	xRtosSemaphoreTake(&shSLvars, portMAX_DELAY);
	if (sRpt.crc == sMsg.crc && sRpt.pri == sMsg.pri) {	// CRC & PRI same as previous message ?
		u16_t Count = ++sRpt.count;						// Yes, increment the repeat counter
		sRpt = sMsg;									// current message info now basis of next repeat
		sRpt.count = Count;								// and update with incremented count
		xRtosSemaphoreGive(&shSLvars);					// variable changes done, unlock and return
		return;
	}
	// Different CRC and/or PRI
	sl_vars_t sPrv = sRpt;								// save previous repeat values for message creation
	sRpt = sMsg;										// save as repeat test for next message
	xRtosSemaphoreGive(&shSLvars);						// variable changes done, unlock and continue
	va_fake_t vaFake = { .pa = NULL };

	// step 5: handle console message(s)
	if (sPrv.count)										// repeated message
		xvSyslogConsole(&sPrv, NULL, vaFake.va);
	xvSyslogConsole(&sMsg, format, vaList);

	// step 6: handle host message(s)
	if ((MsgPRI & 7) <= xSyslogGetHostLevel()) {		// filter based on higher priorities
		if (sPrv.count)
			xvSyslogHost(&sPrv, NULL, vaFake.va);
		xvSyslogHost(&sMsg, format, vaList);
	}

}

void IRAM_ATTR vSyslog(int MsgPRI, const char *FuncID, const char *format, ...) {
	va_list vaList;
	va_start(vaList, format);
	xvSyslog(MsgPRI, FuncID, format, vaList);
	va_end(vaList);
}

int IRAM_ATTR xSyslogError(const char *FuncID, int iRV) {
	vSyslog(SL_SEV_ERROR, FuncID, "iRV=%d (%s)", iRV, pcStrError(iRV));
	return (iRV > 0) ? -iRV : iRV;
}

void vSyslogReport(report_t * psR) {
	if (sCtx.sd <= 0)
		return;
	xNetReport(psR, &sCtx, "SLOG", 0, 0, 0);
	xReport(psR, "\tmaxTX=%zu  CurRpt=%lu" strNL, sCtx.maxTx, sRpt.count);
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
