#pragma once
#include <string>
#include <thread>
#include <mutex>
#include <winsock.h>
#include "utils.h"

#pragma comment(lib, "ws2_32.lib")
using namespace std;
typedef unsigned long DWORD;

class SenderSocket
{
public:
	bool dataFlag = false;
	char* host;
	char* queue;
	char* buffer;
	const char* ip;
	int w,nextSeq,sndBase,fastRetxNum,timeoutNum,lastReleased,it,finalSeq,dataTxed,effectiveWin,pkts,RTTCount;
	double RTT, fl, rl, bn, rto, estRTT, devRTT, prevEstRTT, prevDevRTT, transTime, transSpeed, transRTT, sumRTT;
	DWORD timeStart,timerStart,transStart;
	DWORD* sampStart;
	SOCKET sock;
	Parameters param;
	HANDLE ths[2];
	struct sockaddr_in local, remote;
	SenderSocket(char* host, int window, double r, double f, double rloss, double b);
	~SenderSocket();
	void Open();
	void Close();
	void consSYN();
	void consFIN();
	void consData(char* buf, int length);
	void Send(char* buf, int length);
	void tranPacket(int num);
	void test();
	//int Close();
};
void errorCheck();
void Worker(LPVOID pParam);
void Stats(LPVOID pParam);