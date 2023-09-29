# SYSLOG

set( srcs "syslog.c" )
set( include_dirs "." )
set( requires )
set( priv_requires confidential common hal_esp32 main printfx rtos-support socketsX )

idf_component_register(
	SRCS ${srcs} 
	INCLUDE_DIRS ${include_dirs} 
	REQUIRES ${requires} 
	PRIV_REQUIRES ${priv_requires}
)
