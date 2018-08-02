# Highly functional syslog module specifically for embedded systems

Support for logging to console and syslog server.

Does not use dynamic memory allocation.

Suppress repetitive messages to minimise console output.

Automatically detect the calling FReeRTOS task and use the name thereof as ProcID in syslog message.

Optimised to work with the enhanced and optimised printf module in repo ksstech/printf.

Can be adapted to use normal printf library with minimal effort.

Additional support added for ESP-IDF to integrate into the LOGx macros.
