# Highly functional syslog module specifically for embedded systems

Support for logging to console and syslog server.

Does not use dynamic memory allocation.

Suppress repetitive messages to minimise console output.

Automatically detect the calling FReeRTOS task and use the name thereof as ProcID in syslog message.

Optimised to work with the enhanced and optimised printf module in repo ksstech/printf.

Can be adapted to use normal printf library with minimal effort.

Additional support added for ESP-IDF to integrate into the LOGx macros.

## Background on format
 SYSLOG-MSG = HEADER SP STRUCTURED-DATA [SP MSG]

 HEADER = PRI VERSION SP TIMESTAMP SP HOSTNAME SP APP-NAME SP PROCID SP MSGID

 PRI = "<" PRIVAL ">"
	PRIVAL = 1*3DIGIT ; range 0 .. 191

 VERSION = NONZERO-DIGIT 0*2DIGIT

 TIMESTAMP = NILVALUE / FULL-DATE "T" FULL-TIME

 FULL-DATE =	DATE-FULLYEAR "-" DATE-MONTH "-" DATE-MDAY
				DATE-FULLYEAR = 4DIGIT
				DATE-MONTH = 2DIGIT ; 01-12
				DATE-MDAY = 2DIGIT ; 01-28, 01-29, 01-30, 01-31 based on ; month/year
 FULL-TIME =	PARTIAL-TIME TIME-OFFSET
				PARTIAL-TIME		= TIME-HOUR ":" TIME-MINUTE ":" TIME-SECOND [TIME-SECFRAC]
					TIME-HOUR		= 2DIGIT ; 00-23
					TIME-MINUTE		= 2DIGIT ; 00-59
					TIME-SECOND		= 2DIGIT ; 00-59
					TIME-SECFRAC	= "." 1*6DIGIT
				TIME-OFFSET			= "Z" / TIME-NUMOFFSET
					TIME-NUMOFFSET	= ("+" / "-") TIME-HOUR ":" TIME-MINUTE

 HOSTNAME = NILVALUE / 1*255PRINTUSASCII		(MAC address)

 APP-NAME = NILVALUE / 1*48PRINTUSASCII			(task name)

 PROCID = NILVALUE / 1*128PRINTUSASCII			(core ID)

 MSGID = NILVALUE / 1*32PRINTUSASCII			(function name)

 STRUCTURED-DATA = NILVALUE / 1*SD-ELEMENT
	SD-ELEMENT		= "[" SD-ID *(SP SD-PARAM) "]"
		SD-PARAM	= PARAM-NAME "=" %d34 PARAM-VALUE %d34
		SD-ID		= SD-NAME
		PARAM-NAME	= SD-NAME
		PARAM-VALUE	= UTF-8-STRING ; characters �"�, �\� and ; �]� MUST be escaped.
		SD-NAME		= 1*32PRINTUSASCII ; except �=�, SP, �]�, %d34 (")

 MSG = MSG-ANY / MSG-UTF8
	MSG-ANY		= *OCTET ; not starting with BOM
 	MSG-UTF8	= BOM UTF-8-STRING
		BOM		= %xEF.BB.BF

UTF-8-STRING	= *OCTET ; UTF-8 string as specified ; in RFC 3629
	OCTET		= %d00-255
 SP				= %d32
 PRINTUSASCII	= %d33-126
 NONZERO-DIGIT	= %d49-57
 DIGIT			= %d48 / NONZERO-DIGIT
 NILVALUE		= "-"
