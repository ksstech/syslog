# SYSLOG

set( srcs "syslog.c" )
set( include_dirs "." )
set( priv_include_dirs )
set( requires "confidential hal_esp32 socketsX" )
set( priv_requires "printfx" )

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS ${include_dirs}
	PRIV_INCLUDE_DIRS ${priv_include_dirs}
	REQUIRES ${requires}
	PRIV_REQUIRES ${priv_requires}
)
