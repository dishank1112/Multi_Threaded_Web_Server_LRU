#include "proxy_parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <time.h>

#pragma comment(lib, "Ws2_32.lib")

#define MAX_BYTES 4096
#define MAX_CLIENTS 400
#define MAX_SIZE 200 * (1 << 20)
#define MAX_ELEMENT_SIZE 10 * (1 << 20)

typedef struct cache_element cache_element;

struct cache_element
{
    char *data;
    int len;
    char *url;
    time_t lru_time_track;
    cache_element *next;
};

cache_element *find(char *url);
int add_cache_element(char *data, int size, char *url);
void remove_cache_element();

int port_number = 8080;
SOCKET proxy_socketId;
HANDLE threadHandles[MAX_CLIENTS];
HANDLE semaphore;
CRITICAL_SECTION cacheLock;

cache_element *head;
int cache_size;
int sendErrorMessage(SOCKET socket, int status_code)
{
    char str[1024];
    char currentTime[50];
    time_t now = time(0);
    struct tm *data = gmtime(&now);

    if (data == NULL)
    {
        // Handle gmtime error
        return -1;
    }

    strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", data);

    switch (status_code)
    {
    case 400:
        snprintf(str, sizeof(str), "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: close\r\nContent-Type: text/html\r\nDate: %s\r\nServer: ProxyServer\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>", currentTime);
        send(socket, str, strlen(str), 0); // Check return value if necessary
        break;

    default:
        return -1;
    }
    return 1;
}

SOCKET connectRemoteServer(const char *host_addr, int port_num)
{
    SOCKET remoteSocket = socket(AF_INET, SOCK_STREAM, 0);
    if (remoteSocket == INVALID_SOCKET)
    {
        printf("Error in Creating Socket: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }

    struct hostent *host = gethostbyname(host_addr);
    if (host == NULL)
    {
        fprintf(stderr, "No such host exists.\n");
        return INVALID_SOCKET;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port_num);
    memcpy(&server_addr.sin_addr.s_addr, host->h_addr, host->h_length);

    if (connect(remoteSocket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        fprintf(stderr, "Error in connecting: %d\n", WSAGetLastError());
        return INVALID_SOCKET;
    }
    return remoteSocket;
}
DWORD WINAPI thread_fn(LPVOID socketNew)
{
    WaitForSingleObject(semaphore, INFINITE);
    SOCKET clientSocket = *(SOCKET *)socketNew;
    free(socketNew); // Free memory allocated for the socket pointer

    char buffer[MAX_BYTES] = {0};
    int bytesReceived = recv(clientSocket, buffer, MAX_BYTES, 0);
    if (bytesReceived <= 0)
    {
        closesocket(clientSocket);
        ReleaseSemaphore(semaphore, 1, NULL);
        return 0; // Exit if there's an error
    }

    char url[1024], method[10];
    if (parse_request(buffer, url, method) < 0)
    {
        sendErrorMessage(clientSocket, 400);
        goto cleanup;
    }

    EnterCriticalSection(&cacheLock);
    cache_element *cachedData = find(url);
    LeaveCriticalSection(&cacheLock);

    if (cachedData != NULL)
    {
        send(clientSocket, cachedData->data, cachedData->len, 0);
    }
    else
    {
        SOCKET remoteSocket = connectRemoteServer("example.com", 80);
        if (remoteSocket == INVALID_SOCKET)
        {
            sendErrorMessage(clientSocket, 502);
            goto cleanup;
        }

        send(remoteSocket, buffer, bytesReceived, 0);
        char serverResponse[MAX_SIZE] = {0};
        int responseSize = recv(remoteSocket, serverResponse, MAX_SIZE, 0);

        if (responseSize > 0)
        {
            send(clientSocket, serverResponse, responseSize, 0);
            if (responseSize < MAX_ELEMENT_SIZE)
            {
                EnterCriticalSection(&cacheLock);
                add_cache_element(serverResponse, responseSize, url);
                LeaveCriticalSection(&cacheLock);
            }
        }
        closesocket(remoteSocket);
    }

cleanup:
    ReleaseSemaphore(semaphore, 1, NULL);
    closesocket(clientSocket);
    return 0;
}

int main(){
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("WSAStartup failed: %d\n", WSAGetLastError());
        return 1;
    }

    proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
    if (proxy_socketId == INVALID_SOCKET)
    {
        printf("Socket creation failed: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    struct sockaddr_in proxy_addr;
    memset(&proxy_addr, 0, sizeof(proxy_addr));
    proxy_addr.sin_family = AF_INET;
    proxy_addr.sin_port = htons(port_number);
    proxy_addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(proxy_socketId, (struct sockaddr *)&proxy_addr, sizeof(proxy_addr)) == SOCKET_ERROR)
    {
        printf("Binding failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        WSACleanup();
        return 1;
    }

    if (listen(proxy_socketId, MAX_CLIENTS) == SOCKET_ERROR)
    {
        printf("Listening failed: %d\n", WSAGetLastError());
        closesocket(proxy_socketId);
        WSACleanup();
        return 1;
    }

    semaphore = CreateSemaphore(NULL, MAX_CLIENTS, MAX_CLIENTS, NULL);
    InitializeCriticalSection(&cacheLock);

    printf("Proxy server started on port %d\n", port_number);

    while (1){
         
        SOCKET clientSocket = accept(proxy_socketId, NULL, NULL);
        if (clientSocket == INVALID_SOCKET)
        {
            printf("Accept failed: %d\n", WSAGetLastError());
            continue;
        }
         SOCKET *socketPtr = malloc(sizeof(SOCKET)); // Allocate memory for the socket pointer
        *socketPtr = clientSocket;
        DWORD threadId;
        HANDLE hThread = CreateThread(NULL, 0, thread_fn, &clientSocket, 0, &threadId);
        if (hThread == NULL)
        {
            printf("Thread creation failed: %lu\n", GetLastError());
            closesocket(clientSocket);
            free(socketPtr);
        }
        else
        {
            CloseHandle(hThread);
        }
    }

    CloseHandle(semaphore);
    DeleteCriticalSection(&cacheLock);
    closesocket(proxy_socketId);
    WSACleanup();

    return 0;
}
