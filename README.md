# proxy

Concurrent http proxy written in C, works in browser and through telnet. Developed by me and Chris Lee. Main code file is proxy.c

Uses a shared buffer struct and mutexes to ensure that parallel proxy connections can work up to a specified number of connections. 
Shared buffer stores request info from main thread, and concurrent threads will pick up a request and fulfill it, with the help of mutex locks
to ensure no deadlocks nor concurrent writes happen in the buffer.

Will log each entry on top of printing to console. Opens a listening socket on the specified TCP port number.  Runs forever
accepting client connections.  Echoes lines read from a connection until the connection is closed by the client.  Only accepts a new connection
after the old connection is closed. Checks all sorts of HTTP errors specified by mozilla.



