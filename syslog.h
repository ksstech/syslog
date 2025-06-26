// syslog.h

#pragma once

#include "sdkconfig.h"
#include "report.h"

#include "FreeRTOS_Support.h"

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ########################################### macros ##############################################

#define SL_MAX_LEN_MESSAGE			1024

// ############################# Facilities & Severities definitions ###############################

#define		SL_FAC_KERNEL			0			// kernel messages
#define		SL_FAC_USER				1			// user-level messages
#define		SL_FAC_MAIL				2			// mail system
#define		SL_FAC_SYSTEM			3			// system daemons
#define		SL_FAC_SECURITY			4			// security/authorization messages
#define		SL_FAC_SYSLOG			5			// messages generated internally by syslogd
#define		SL_FAC_LINEPRNTR		6			// line printer subsystem
#define		SL_FAC_NEWS				7			// network news subsystem
#define		SL_FAC_UUCP				8			// UUCP subsystem
#define		SL_FAC_CLOCK			9			// clock daemon
#define		SL_FAC_SECURITY2		10			// security/authorization messages
#define		SL_FAC_FTP				11			// FTP daemon
#define		SL_FAC_NTP				12			// NTP subsystem
#define		SL_FAC_LOGAUDIT			13			// log audit
#define		SL_FAC_LOGALERT			14			// log alert
#define		SL_FAC_CLOCK2			15			// clock daemon (note 2)
#define		SL_FAC_LOCAL0			16			// local use 0 (local0)
#define		SL_FAC_LOCAL1			17			// local use 1 (local1)
#define		SL_FAC_LOCAL2			18			// local use 2 (local2)
#define		SL_FAC_LOCAL3			19			// local use 3 (local3)
#define		SL_FAC_LOCAL4			20			// local use 4 (local4)
#define		SL_FAC_LOCAL5			21			// local use 5 (local5)
#define		SL_FAC_LOCAL6			22			// local use 6 (local6)
#define		SL_FAC_LOCAL7			23			// local use 7 (local7)

#define		SL_SEV_EMERGENCY		0			// Emergency: system is unusable
#define		SL_SEV_ALERT			1			// Alert: action must be taken immediately
#define		SL_SEV_CRITICAL			2			// Critical: critical conditions
#define		SL_SEV_ERROR			3			// Error: error conditions
#define		SL_SEV_WARNING			4			// Warning: warning conditions
#define		SL_SEV_NOTICE			5			// Notice: normal but significant condition
#define		SL_SEV_INFO				6			// Informational: informational messages
#define		SL_SEV_DEBUG			7			// Debug: debug-level messages

// '<7>1 2021/10/21T12:34.567: cc50e38819ec_WROVERv4_5C9 #0 esp_timer halVARS_Report????? - '
#define slSIZEBUF					512
#define slFILESIZE					10204				// MAX history (at boot) size before truncation
#define UNKNOWNMACAD				"#UnknownMAC#"		// MAC address marker in pre-wifi messages
#define slFILENAME					"/syslog.txt"		// default file name in root directory

#define slMS_LOCK_WAIT				200					/* was 1000 */
#define slMS_FILESEND_DLY			5

// ############################## Syslog formatting/calling macros #################################

#define SL_PRI(fac,sev)				(((fac)<<3) | ((sev)&7))
#define SL_LOG(pri, f, ...) 		do { 												\
										if (((pri)&7) <= SL_LEV_MAX) {					\
											 vSyslog(pri,__FUNCTION__,f,##__VA_ARGS__);	\
										} 												\
									} while(0)

#define SL_ERROR(err) 				xSyslogError(__FUNCTION__, err)
#define	SL_EMER(fmt, ...)			SL_LOG(SL_SEV_EMERGENCY, fmt, ##__VA_ARGS__)
#define	SL_ALRT(fmt, ...)			SL_LOG(SL_SEV_ALERT, fmt, ##__VA_ARGS__)
#define	SL_CRIT(fmt, ...)			SL_LOG(SL_SEV_CRITICAL, fmt, ##__VA_ARGS__)
#define	SL_ERR(fmt, ...)			SL_LOG(SL_SEV_ERROR, fmt, ##__VA_ARGS__)
#define	SL_WARN(fmt, ...)			SL_LOG(SL_SEV_WARNING, fmt, ##__VA_ARGS__)
#define	SL_NOT(fmt, ...)			SL_LOG(SL_SEV_NOTICE, fmt, ##__VA_ARGS__)
#define	SL_INFO(fmt, ...)			SL_LOG(SL_SEV_INFO, fmt, ##__VA_ARGS__)
#define	SL_DBG(fmt, ...)			SL_LOG(SL_SEV_DEBUG, fmt, ##__VA_ARGS__)

#define IF_SL_ERROR(tst, err) 		if (tst) SL_ERROR(err)
#define	IF_SL_EMER(tst, fmt, ...)	if (tst) SL_EMER(fmt, ##__VA_ARGS__)
#define	IF_SL_ALRT(tst, fmt, ...)	if (tst) SL_ALRT(fmt, ##__VA_ARGS__)
#define	IF_SL_CRIT(tst, fmt, ...)	if (tst) SL_CRIT(fmt, ##__VA_ARGS__)
#define	IF_SL_ERR(tst, fmt, ...)	if (tst) SL_ERR(fmt, ##__VA_ARGS__)
#define	IF_SL_WARN(tst, fmt, ...)	if (tst) SL_WARN(fmt, ##__VA_ARGS__)
#define	IF_SL_NOT(tst, fmt, ...)	if (tst) SL_NOT(fmt, ##__VA_ARGS__)
#define	IF_SL_INFO(tst, fmt, ...)	if (tst) SL_INFO(fmt, ##__VA_ARGS__)
#define	IF_SL_DBG(tst, fmt, ...)	if (tst) SL_DBG(fmt, ##__VA_ARGS__)

// ###################################### Global variables #########################################

extern SemaphoreHandle_t shSLsock, shSLvars;			// public to enable semaphore un/lock tracking

// ###################################### function prototypes ######################################

/**
 * @brief	Check if port# matches syslog context, close the socket if different from conext socket
 * @param[in]	sock open socket number being investigated
 * @param[in]	addr IP address of open socket being investigated
 * @return	0 if not matching/closed, 1 if port match but different socket hence closed
 */
struct sockaddr_in;
int xSyslogCheckDuplicates(int sock, struct sockaddr_in * addr);

/**
 * @brief
 * @return
 */
int xSyslogGetConsoleLevel(void);
int xSyslogGetHostLevel(void);

/**
 * @brief
 * @param[in]
 */
void vSyslogSetConsoleLevel(int Level);
void vSyslogSetHostLevel(int Level);

/**
 * @brief	Check size of slFILENAME, if bigger than slFILESIZE will unlink/delete the file to make space
*/
void vSyslogFileCheckSize(void);

/**
 * @brief		writes an RFC formatted message to stdout & syslog host (if up and running)
 * @param[in]	MsgPRI PRIority (combined FACility & SEVerity)
 * @param[in]	FuncID originating function name
 * @param[in]	format string and parameters as per normal printf()
 * @return		number of characters displayed(if only to console) or send(if to server)
*/
void xvSyslog(int MsgPRI, const char * FuncID, const char * format, va_list args);

/**
 * @brief		writes an RFC formatted message to stdout & syslog host (if up and running)
 * @param[in]	MsgPRI PRIority (combined FACility & SEVerity)
 * @param[in]	FuncID originating function name
 * @param[in]	format string and parameters as per normal printf()
 * @param[in]	varargs variable number of arguments
 * @return		number of characters displayed(if only to console) or send(if to server)
*/
void vSyslog(int MsgPRI, const char * FuncID, const char * format, ...);

/**
 * @brief
 * @param[in]	pcFN function where error occurred, invoking this handler
 * @param[in]	eCode error code to be mapped to a syslog message with pri ERROR
 * @return
 */
int xSyslogError(const char * FuncID, int eCode);

/**
 * @brief	report syslog related information
 * @param[in]	psR pointer to report structure
*/
void vSyslogReport(report_t * psR);

#ifdef __cplusplus
}
#endif
