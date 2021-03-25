/*
 * Copyright 2014-21 Andre M. Maree / KSS Technologies (Pty) Ltd.
 */

#pragma once

#include	"hal_config.h"

#include	<stdarg.h>
#include	<stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ############################################ macros ############################################

#define	syslogHOSTNAME				"host.domain.tld"

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

// ############################## Syslog formatting/calling macros #################################

#define	SL_LEVEL					(CONFIG_LOG_DEFAULT_LEVEL + 2)
#define	SL_MOD2LOCAL(SEV)			((SL_FAC_LOCAL0 << 3) | (SEV) )

#define	SL_LOG(x, f, ...) 			do { \
	if (SL_LEVEL >= (x)) \
		xSyslog(SL_MOD2LOCAL(x) , __FUNCTION__ ,f , ##__VA_ARGS__) ; \
	} while(0)

#define	SL_EMER(f, ...)				SL_LOG(SL_SEV_EMERGENCY, f, ##__VA_ARGS__)
#define	SL_ALRT(f, ...)				SL_LOG(SL_SEV_ALERT, f, ##__VA_ARGS__)
#define	SL_CRIT(f, ...)				SL_LOG(SL_SEV_CRITICAL, f, ##__VA_ARGS__)
#define	SL_ERR(f, ...)				SL_LOG(SL_SEV_ERROR, f, ##__VA_ARGS__)
#define	SL_WARN(f, ...)				SL_LOG(SL_SEV_WARNING, f, ##__VA_ARGS__)
#define	SL_NOT(f, ...)				SL_LOG(SL_SEV_NOTICE, f, ##__VA_ARGS__)
#define	SL_INFO(f, ...)				SL_LOG(SL_SEV_INFO, f, ##__VA_ARGS__)
#define	SL_DBG(f, ...)				SL_LOG(SL_SEV_DEBUG, f, ##__VA_ARGS__)

#define	IF_SL_EMER(x, f, ...)		if (x) SL_EMER(f, ##__VA_ARGS__)
#define	IF_SL_ALRT(x, f, ...)		if (x) SL_ALRT(f, ##__VA_ARGS__)
#define	IF_SL_CRIT(x, f, ...)		if (x) SL_CRIT(f, ##__VA_ARGS__)
#define	IF_SL_ERR(x, f, ...)		if (x) SL_ERR(f, ##__VA_ARGS__)
#define	IF_SL_WARN(x, f, ...)		if (x) SL_WARN(f, ##__VA_ARGS__)
#define	IF_SL_NOT(x, f, ...)		if (x) SL_NOT(f, ##__VA_ARGS__)
#define	IF_SL_INFO(x, f, ...)		if (x) SL_INFO(f, ##__VA_ARGS__)
#define	IF_SL_DBG(x, f, ...)		if (x) SL_DBG(f, ##__VA_ARGS__)

// ###################################### Global variables #########################################


// ###################################### function prototypes ######################################

int32_t	xSyslogInit(const char * pcHostName, uint64_t * ptRunTime, uint64_t * ptUTCTime) ;
int32_t	xSyslogConnect(void) ;
void	vSyslogDisConnect(void) ;

void 	vSyslogSetPriority(uint32_t Priority) ;
int32_t	xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list args) ;
int32_t	xSyslog(uint32_t Priority, const char * MsgID, const char * format, ...) ;

void	vSyslogReport(void) ;

#ifdef __cplusplus
}
#endif
