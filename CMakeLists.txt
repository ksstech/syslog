# SYSLOG

set( srcs "syslog.c" )
set( include_dirs "." )
set( priv_include_dirs )
set( requires "options printfx rtos-support")
set( priv_requires "common hal_esp32 socketsX" )

idf_component_register(
	SRCS ${srcs}
	INCLUDE_DIRS ${include_dirs}
	PRIV_INCLUDE_DIRS ${priv_include_dirs}
	REQUIRES ${requires}
	PRIV_REQUIRES ${priv_requires}
)
