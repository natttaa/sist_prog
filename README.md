cl /EHsc server.cpp /Fe:server.exe
cl /EHsc client.cpp /Fe:client.exe
cl /EHsc service.cpp /link ws2_32.lib advapi32.lib
