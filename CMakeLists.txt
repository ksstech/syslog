idf_component_register( SRCS x_syslog.c 
						INCLUDE_DIRS . 
						REQUIRES common
						PRIV_REQUIRES common printf x_sockets hal_esp32
						)
