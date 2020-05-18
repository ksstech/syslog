idf_component_register( SRCS syslog.c 
						INCLUDE_DIRS . 
						REQUIRES common
						PRIV_REQUIRES common printf x_sockets hal_esp32
						)
