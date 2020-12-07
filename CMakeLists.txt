idf_component_register(
	SRCS "syslog.c"
	INCLUDE_DIRS "."
	REQUIRES common hal_esp32
	PRIV_REQUIRES rtos-support printfx socketsX
	)
