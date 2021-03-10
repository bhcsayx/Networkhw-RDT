#pragma once
#include <iostream>
#include <string>
#include <winsock.h>

#pragma comment(lib, "ws2_32.lib")

#define MAGIC_PROTOCOL 0x8311AA
using namespace std;
typedef unsigned long DWORD;

class Flags {
public:
	DWORD reserved : 5; // must be zero
	DWORD SYN : 1;
	DWORD ACK : 1;
	DWORD FIN : 1;
	DWORD magic : 24;
	Flags() { memset(this, 0, sizeof(*this)); magic = MAGIC_PROTOCOL; }
};

class SenderDataHeader {
public:
	Flags flags;
	DWORD seq; // must begin from 0
	SenderDataHeader() { seq = 0; };
	SenderDataHeader(int s) { seq = s; };
};

class LinkProperties {
public:
	// transfer parameters
	float RTT; // propagation RTT (in sec)
	float speed; // bottleneck bandwidth (in bits/sec)
	float pLoss[2]; // probability of loss in each direction
	DWORD bufferSize; // buffer size of emulated routers (in packets)
	LinkProperties() { memset(this, 0, sizeof(*this)); }
};

class SenderSynHeader {
public:
	SenderDataHeader sdh;
	LinkProperties lp;
	SenderSynHeader(SenderDataHeader s, LinkProperties l) { sdh = s; lp = l; };
};

class ReceiverHeader {
public:
	Flags flags;
	DWORD recvWnd; // receiver window for flow control (in pkts) 
	DWORD ackSeq; // ack value = next expected sequence
};

class Parameters {
public:
	HANDLE  mutex;
	HANDLE	finished;
	HANDLE  workerQuit;
	HANDLE	eventQuit;
	HANDLE  empty;
	HANDLE  full;
};

class Packet {
public:
	int seq; // for easy access in worker thread
	int type; // SYN, FIN, data
	int size; // for retx in worker thread
};