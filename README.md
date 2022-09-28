# I/O multiplexing for concurrent connection handling

# The server

## How to compile
```
g++ -Wall -Wextra -o server-io server-io-multiplexing-skeleton.cpp
```

## How to run
```
./server-io <remote port>
```
For exemple :
```
./server-io 5703
```

# The client

## How to compile

### Simple client
```
g++ -Wall -Wextra -o client-simple client-simple.cpp
```

### Multi client
```
g++ -Wall -Wextra -o client-multi client-multi.cpp
```

## How to run

### Simple client
```
./client-simple <remote host> <remote port>
```
For exemple :
```
./client-simple 0.0.0.0 5703
```

### Multi client
```
./client-multi  <remote host> <remote port> <number of concurrent connections> [number of times the message is sent] 
```
For exemple :
```
./client-multi 0.0.0.0 5703 255 1705 
```