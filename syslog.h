// syslog.h

#pragma once

#include <stdarg.h>
#include "sdkconfig.h"
#include "FreeRTOS_Support.h"

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

#ifndef SL_LEVEL_MAX
	#define SL_LEVEL_MAX			7
#endif

// ############################## Syslog formatting/calling macros #################################

#define SL_PRI(fac,sev)				((fac << 3) | (sev & 0x07))
#define SL_LOG(p,f,...) 			do { if (((p & 7)) <= SL_LEVEL_MAX) vSyslog(p,__FUNCTION__,f,##__VA_ARGS__); } while(0)
#define SL_ERROR(e) 				xSyslogError(__FUNCTION__, e)
#define IF_SL_ERROR(t,e) 			if (t) xSyslogError(__FUNCTION__, e)

#define	SL_EMER(fmt, ...)			SL_LOG(SL_SEV_EMERGENCY, fmt, ##__VA_ARGS__)
#define	SL_ALRT(fmt, ...)			SL_LOG(SL_SEV_ALERT, fmt, ##__VA_ARGS__)
#define	SL_CRIT(fmt, ...)			SL_LOG(SL_SEV_CRITICAL, fmt, ##__VA_ARGS__)
#define	SL_ERR(fmt, ...)			SL_LOG(SL_SEV_ERROR, fmt, ##__VA_ARGS__)
#define	SL_WARN(fmt, ...)			SL_LOG(SL_SEV_WARNING, fmt, ##__VA_ARGS__)
#define	SL_NOT(fmt, ...)			SL_LOG(SL_SEV_NOTICE, fmt, ##__VA_ARGS__)
#define	SL_INFO(fmt, ...)			SL_LOG(SL_SEV_INFO, fmt, ##__VA_ARGS__)
#define	SL_DBG(fmt, ...)			SL_LOG(SL_SEV_DEBUG, fmt, ##__VA_ARGS__)

#define	IF_SL_EMER(tst, fmt, ...)	if (tst) SL_EMER(fmt, ##__VA_ARGS__)
#define	IF_SL_ALRT(tst, fmt, ...)	if (tst) SL_ALRT(fmt, ##__VA_ARGS__)
#define	IF_SL_CRIT(tst, fmt, ...)	if (tst) SL_CRIT(fmt, ##__VA_ARGS__)
#define	IF_SL_ERR(tst, fmt, ...)	if (tst) SL_ERR(fmt, ##__VA_ARGS__)
#define	IF_SL_WARN(tst, fmt, ...)	if (tst) SL_WARN(fmt, ##__VA_ARGS__)
#define	IF_SL_NOT(tst, fmt, ...)	if (tst) SL_NOT(fmt, ##__VA_ARGS__)
#define	IF_SL_INFO(tst, fmt, ...)	if (tst) SL_INFO(fmt, ##__VA_ARGS__)
#define	IF_SL_DBG(tst, fmt, ...)	if (tst) SL_DBG(fmt, ##__VA_ARGS__)

// ###################################### Global variables #########################################

extern SemaphoreHandle_t SL_NetMux, SL_VarMux;			// public to enable semaphore un/lock tracking

// ###################################### function prototypes ######################################

int xSyslogGetConsoleLevel(void);
int xSyslogGetHostLevel(void);
void vSyslogSetConsoleLevel(int Level);

void vSyslogFileCheckSize(void);
void xvSyslog(int Priority, const char * MsgID, const char * format, va_list args);
void vSyslog(int Priority, const char * MsgID, const char * format, ...);
int xSyslogError(const char * pcFN, int eCode);

struct report_t;
void vSyslogReport(struct report_t * psR);

#ifdef __cplusplus
}
#endif
