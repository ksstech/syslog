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
 */

/*
 * x_syslog.h
 */

#pragma once

#include	"x_printf.h"
#include	"x_sockets.h"
#include	"x_definitions.h"

#include	<stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

// ############################################ macros ############################################

//#define	syslogHOSTNAME					"host.domain.tld"

// ############################# Facilities & Severities definitions ###############################

#define		SL_FAC_KERNEL				0			// kernel messages
#define		SL_FAC_USER					1			// user-level messages
#define		SL_FAC_MAIL					2			// mail system
#define		SL_FAC_SYSTEM				3			// system daemons
#define		SL_FAC_SECURITY				4			// security/authorization messages
#define		SL_FAC_SYSLOG				5			// messages generated internally by syslogd
#define		SL_FAC_LINEPRNTR			6			// line printer subsystem
#define		SL_FAC_NEWS					7			// network news subsystem
#define		SL_FAC_UUCP					8			// UUCP subsystem
#define		SL_FAC_CLOCK				9			// clock daemon
#define		SL_FAC_SECURITY2			10			// security/authorization messages
#define		SL_FAC_FTP					11			// FTP daemon
#define		SL_FAC_NTP					12			// NTP subsystem
#define		SL_FAC_LOGAUDIT				13			// log audit
#define		SL_FAC_LOGALERT				14			// log alert
#define		SL_FAC_CLOCK2				15			// clock daemon (note 2)
#define		SL_FAC_LOCAL0				16			// local use 0 (local0)
#define		SL_FAC_LOCAL1				17			// local use 1 (local1)
#define		SL_FAC_LOCAL2				18			// local use 2 (local2)
#define		SL_FAC_LOCAL3				19			// local use 3 (local3)
#define		SL_FAC_LOCAL4				20			// local use 4 (local4)
#define		SL_FAC_LOCAL5				21			// local use 5 (local5)
#define		SL_FAC_LOCAL6				22			// local use 6 (local6)
#define		SL_FAC_LOCAL7				23			// local use 7 (local7)

#define		SL_SEV_EMERGENCY			0			// Emergency: system is unusable
#define		SL_SEV_ALERT				1			// Alert: action must be taken immediately
#define		SL_SEV_CRITICAL				2			// Critical: critical conditions
#define		SL_SEV_ERROR				3			// Error: error conditions
#define		SL_SEV_WARNING				4			// Warning: warning conditions
#define		SL_SEV_NOTICE				5			// Notice: normal but significant condition
#define		SL_SEV_INFO					6			// Informational: informational messages
#define		SL_SEV_DEBUG				7			// Debug: debug-level messages

// ############################## Syslog formatting/calling macros #################################

#define	SL_CHECK(x)						if (x < erSUCCESS) xSyslog(SL_MOD2LOCAL(SL_SEV_ERROR), NULL, __FUNCTION__, "()=%d", x)
#define	SL_MOD2LOCAL(SEV)				((SL_FAC_LOGAUDIT << 3) | SEV)

#define	SL_EMER(FORMAT, ...)			xSyslog(SL_MOD2LOCAL(SL_SEV_EMERGENCY),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_ALRT(FORMAT, ...)			xSyslog(SL_MOD2LOCAL(SL_SEV_ALERT),		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_CRIT(FORMAT, ...)			xSyslog(SL_MOD2LOCAL(SL_SEV_CRITICAL),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_ERR(FORMAT, ...)				xSyslog(SL_MOD2LOCAL(SL_SEV_ERROR),		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_WARN(FORMAT, ...)			xSyslog(SL_MOD2LOCAL(SL_SEV_WARNING),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_NOT(FORMAT, ...)				xSyslog(SL_MOD2LOCAL(SL_SEV_NOTICE), 	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_INFO(FORMAT, ...)			xSyslog(SL_MOD2LOCAL(SL_SEV_INFO), 		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	SL_DBG(FORMAT, ...)				xSyslog(SL_MOD2LOCAL(SL_SEV_DEBUG),		__FUNCTION__, FORMAT, ##__VA_ARGS__)

#define	IF_SL_EMER(x, FORMAT, ...)		if (x) SL_EMER(FORMAT, ##__VA_ARGS__)
#define	IF_SL_ALRT(x, FORMAT, ...)		if (x) SL_ALRT(FORMAT, ##__VA_ARGS__)
#define	IF_SL_CRIT(x, FORMAT, ...)		if (x) SL_CRIT(FORMAT, ##__VA_ARGS__)
#define	IF_SL_ERR(x, FORMAT, ...)		if (x) SL_ERR(FORMAT, ##__VA_ARGS__)
#define	IF_SL_WARN(x, FORMAT, ...)		if (x) SL_WARN(FORMAT, ##__VA_ARGS__)
#define	IF_SL_NOT(x, FORMAT, ...)		if (x) SL_NOT(FORMAT, ##__VA_ARGS__)
#define	IF_SL_INFO(x, FORMAT, ...)		if (x) SL_INFO(FORMAT, ##__VA_ARGS__)
#define	IF_SL_DBG(x, FORMAT, ...)		if (x) SL_DBG(FORMAT, ##__VA_ARGS__)

#define	CL_MOD2LOCAL(SEV)				((SL_FAC_LOCAL0 << 3) | SEV)

#define	CL_EMER(FORMAT, ...)			xSyslog(CL_MOD2LOCAL(SL_SEV_EMERGENCY),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_ALRT(FORMAT, ...)			xSyslog(CL_MOD2LOCAL(SL_SEV_ALERT),		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_CRIT(FORMAT, ...)			xSyslog(CL_MOD2LOCAL(SL_SEV_CRITICAL),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_ERR(FORMAT, ...)				xSyslog(CL_MOD2LOCAL(SL_SEV_ERROR),		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_WARN(FORMAT, ...)			xSyslog(CL_MOD2LOCAL(SL_SEV_WARNING),	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_NOT(FORMAT, ...)				xSyslog(CL_MOD2LOCAL(SL_SEV_NOTICE), 	__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_INFO(FORMAT, ...)			xSyslog(CL_MOD2LOCAL(SL_SEV_INFO), 		__FUNCTION__, FORMAT, ##__VA_ARGS__)
#define	CL_DBG(FORMAT, ...)				xSyslog(CL_MOD2LOCAL(SL_SEV_DEBUG),		__FUNCTION__, FORMAT, ##__VA_ARGS__)

#define	IF_CL_EMER(x, FORMAT, ...)		if (x) CL_EMER(FORMAT, ##__VA_ARGS__)
#define	IF_CL_ALRT(x, FORMAT, ...)		if (x) CL_ALRT(FORMAT, ##__VA_ARGS__)
#define	IF_CL_CRIT(x, FORMAT, ...)		if (x) CL_CRIT(FORMAT, ##__VA_ARGS__)
#define	IF_CL_ERR(x, FORMAT, ...)		if (x) CL_ERR(FORMAT, ##__VA_ARGS__)
#define	IF_CL_WARN(x, FORMAT, ...)		if (x) CL_WARN(FORMAT, ##__VA_ARGS__)
#define	IF_CL_NOT(x, FORMAT, ...)		if (x) CL_NOT(FORMAT, ##__VA_ARGS__)
#define	IF_CL_INFO(x, FORMAT, ...)		if (x) CL_INFO(FORMAT, ##__VA_ARGS__)
#define	IF_CL_DBG(x, FORMAT, ...)		if (x) CL_DBG(FORMAT, ##__VA_ARGS__)

// ###################################### Global variables #########################################


// ###################################### function prototypes ######################################

void 	vSyslogSetPriority(uint32_t Priority) ;
int32_t	xvSyslog(uint32_t Priority, const char * MsgID, const char * format, va_list args) ;
int32_t	xSyslog(uint32_t Priority, const char * MsgID, const char * format, ...) ;

int32_t	xvLog(const char * format, va_list vArgs) ;
int32_t	xLog(const char * format, ...) ;
int32_t	xLogFunc(int32_t (*F)(uint8_t *, size_t)) ;

void	vSyslogReport(void) ;

#ifdef __cplusplus
}
#endif
