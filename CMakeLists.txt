# SYSLOG
set( srcs "syslog.c" )
set( include_dirs "." )
set( requires 
	hal_esp32 
)
set( priv_requires 
	common main printfx rtos-support socketsX 
)
idf_component_register(SRCS ${srcs} INCLUDE_DIRS ${include_dirs} REQUIRES ${requires} PRIV_REQUIRES ${priv_requires})
