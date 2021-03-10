#include <iostream>
#include <string>
#include <winsock2.h>
#include <thread>
#include <mutex>
#include "utils.h"
#include "SenderSocket.h"

#pragma comment(lib, "ws2_32.lib")
using namespace std;
typedef unsigned long DWORD;
#define MAX_PKT_SIZE (1500-28)
SenderSocket::SenderSocket(char* host, int window, double r, double f, double rloss, double b)
{
	//Initialize class parameters
	host = host; pkts = 0; RTTCount = 0; sumRTT = 0;
	w = window; transRTT = INFINITE;
	RTT = r;fl = f; rl = rloss; bn = b;
	nextSeq = 0; sndBase = 0; dataTxed = 0;
	queue = new char[(MAX_PKT_SIZE + sizeof(Packet)) * w];
	buffer = new char[MAX_PKT_SIZE + sizeof(Packet)];
	sampStart = new DWORD[w];
	timeStart = GetTickCount64();

	//starting socket
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		printf("WSAStartup error %d\n", WSAGetLastError());
		WSACleanup();
		exit(EXIT_FAILURE);
	}
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock == INVALID_SOCKET)
	{
		printf("socket creating error,");
		errorCheck();
	}
		

	if (inet_addr(host) == INADDR_NONE)//target host is a hostname
	{
		struct hostent* ht = gethostbyname((const char*)host);
		if (!ht)
		{
			printf("host resolving error,");
			errorCheck();
		}
		else
			ip = inet_ntoa(*((struct in_addr*)ht->h_addr_list[0]));
	}
	else
		ip = host;

	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = INADDR_ANY;
	local.sin_port = htons(0);
	if (bind(sock, (struct sockaddr*)&local, sizeof(local)) == SOCKET_ERROR)
	{
		printf("socket binding error,");
		errorCheck();
	}

	memset(&remote, 0, sizeof(remote));
	remote.sin_family = AF_INET;
	remote.sin_addr.S_un.S_addr = inet_addr(ip);
	remote.sin_port = htons(22345);

	int kernelBuffer = 20e6; // 20 meg
	if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
	{
		printf("Setting sock rcvbuf failed,");
		errorCheck();
	}

	kernelBuffer = 20e6; // 20 meg
	if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (const char*)&kernelBuffer, sizeof(int)) == SOCKET_ERROR)
	{
		printf("Setting sock sndbuf failed,");
		errorCheck();
	}

	//setting socket events
	param.empty = CreateSemaphore(NULL, 0, w, NULL);
	param.full = CreateSemaphore(NULL, 0, w, NULL);
	param.eventQuit = CreateEvent(NULL, true, false, NULL);
	param.workerQuit = CreateEvent(NULL, true, false, NULL);
	param.mutex = CreateMutex(NULL, 0, NULL);
	//starting threads
}

void SenderSocket::consSYN()
{
	Packet p;
	p.seq = 0;
	p.size = sizeof(SenderSynHeader);
	p.type = 0;//0=syn, 1=fin, 2=data

	SenderDataHeader sdh;
	LinkProperties lp;

	sdh.flags.SYN = true;
	lp.bufferSize = w;
	lp.pLoss[0] = fl;
	lp.pLoss[1] = rl;
	lp.RTT = RTT;
	lp.speed = bn * 1e6;

	//printf("for loss: %f\n", fl);
	SenderSynHeader syn(sdh, lp);
	memcpy(buffer, (const char*)&p, sizeof(Packet));
	memcpy(buffer+sizeof(Packet), (const char*)&syn, sizeof(SenderSynHeader));
	return;
}

void SenderSocket::consFIN()
{
	Packet p;
	p.seq = nextSeq;
	p.size = sizeof(SenderDataHeader);
	p.type = 1;//0=syn, 1=fin, 2=data

	SenderDataHeader sdh;
	sdh.flags.FIN = true;
	sdh.seq = nextSeq;
	memcpy(buffer, (const char*)&p, sizeof(Packet));
	memcpy(buffer + sizeof(Packet), (const char*)&sdh, sizeof(SenderDataHeader));
	
	/*for (int i = 0; i < 5; i++)
	{
		printf("%x ", *(int*)(buffer + 4 * i));
	}*/
	return;
}

void SenderSocket::Open()
{
	consSYN(); int i = 0; rto = max(1, 2 * RTT);
	char* recvbuf; recvbuf = new char[12];
	timeval tp; DWORD timerStart, timerEnd;
	if (rto <= 1)
	{
		tp.tv_sec = 0;
		tp.tv_usec = rto * 1e6;
	}
	else
	{
		tp.tv_sec = rto;
		tp.tv_usec = 0;
	}
	while (i < 50)
	{
		timerStart = GetTickCount64();
		if (sendto(sock, (const char*)(buffer + sizeof(Packet)), sizeof(SenderSynHeader), 0, (struct sockaddr*)&remote, sizeof(remote)) == SOCKET_ERROR)
		{
			if (i < 50)
				continue;
			printf("Transmit error,");
			errorCheck();
		}
		else
		{
			int remoteLen = sizeof(remote);
			fd_set fd;
			FD_ZERO(&fd);
			FD_SET(sock, &fd);
			int available = select(0, &fd, NULL, NULL, &tp);
			if (available > 0)
			{
				int recvL = recvfrom(sock, (char*)recvbuf, 12, 0, (struct sockaddr*)&remote, &remoteLen);
				if (recvL == SOCKET_ERROR)
				{
					WSACleanup();
					printf("Error receiving, quitting...\n");
					return;
				}
				else
				{
					timerEnd = GetTickCount64();
					estRTT = (double)(timerEnd - timerStart) / 1000;
					devRTT = 0;
					rto = estRTT + 4 * max(devRTT, 0.010);
					ReceiverHeader* rh = (ReceiverHeader*)recvbuf;
					//printf("recv syn-ack packet with seq %x\n", rh->ackSeq);
					//printf("Initial estRTT %.3f, rto %.3f\n", estRTT, rto);
					nextSeq = rh->ackSeq;
					lastReleased = min(w, rh->recvWnd);
					ReleaseSemaphore(param.empty, lastReleased, NULL);
					transStart = GetTickCount64();
					break;
				}
			}
			else
			{
				cout << "timeout SYN..." << endl;
				if (i < 50)
					i++;
			}
		}
	}
	if (i == 50)
	{
		printf("SYN sending failed, ");
		errorCheck();
	}
	//connection started, start thread
	//ths[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Worker, (void*)this, 0, NULL);
	return;
}

void SenderSocket::Close()
{
	//WaitForSingleObject(ths[0], INFINITE);
	consFIN(); int i = 0;
	char* recbuf; recbuf = new char[12];
	timeval tp; DWORD timerStart, timerEnd;
	if (rto <= 1)
	{
		tp.tv_sec = 0;
		tp.tv_usec = rto * 1e6;
	}
	else
	{
		tp.tv_sec = rto;
		tp.tv_usec = 0;
	}
	tp.tv_sec = 1; tp.tv_usec = 0;
	//transSpeed = (double)dataTxed * 8 / (transTime * 1000);


	while (i < 50)
	{
		timerStart = GetTickCount64();
		if (sendto(sock, (const char*)(buffer + sizeof(Packet)), sizeof(SenderDataHeader), 0, (struct sockaddr*)&remote, sizeof(remote)) == SOCKET_ERROR)
		{
			if (i < 50)
				continue;
			printf("Transmit error,");
			errorCheck();
		}
		else
		{
			int remoteLen = sizeof(remote);
			fd_set fd;
			FD_ZERO(&fd);
			FD_SET(sock, &fd);
			int available = select(0, &fd, NULL, NULL, &tp);
			if (available > 0)
			{
				int recvL = recvfrom(sock, (char*)recbuf, 12, 0, (struct sockaddr*)&remote, &remoteLen);
				if (recvL == SOCKET_ERROR)
				{
					if (i < 50)
						continue;
					WSACleanup();
					printf("Error receiving, quitting...\n");
					return;
				}
				else
				{
					ReceiverHeader* rh = (ReceiverHeader*)recbuf;
					printf("[%.3f] <-- FIN-ACK %d window %x\n", transTime, rh->ackSeq, rh->recvWnd);
					break;
				}
			}
			else
			{
				if (i < 50)
					i++;
			}
		}
	}
	if (i == 50)
	{
		printf("FIN sending failed, ");
		errorCheck();
	}
	//printf("Closed!\n");
	//connection started, start thread
	//ths[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Worker, (void*)this, 0, NULL);
	return;
}

void SenderSocket::consData(char* buf, int length)
{
	//cout << "Packet: " << nextSeq << endl;
	Packet p;
	p.seq = nextSeq;
	p.type = 2;
	p.size = length+sizeof(SenderDataHeader);

	SenderDataHeader sdh;
	sdh.seq = nextSeq;
	memcpy(buffer, (const char*)&p, sizeof(Packet));
	memcpy(buffer + sizeof(Packet), (const char*)&sdh, sizeof(SenderDataHeader));
	memcpy(buffer + sizeof(Packet) + sizeof(SenderDataHeader), buf, length);
	/*cout << "buffer content: ";
	for (int i = 0; i < 5; i++)
	{
		printf("%x ", *(int*)(buffer + 4 * i));
	}
	cout << endl;*/
	return;
}

void SenderSocket::Send(char* buf, int length)
{
	HANDLE arr[2] = { param.empty, param.eventQuit };
	//cout << "waiting empty" << endl;
	int r = WaitForMultipleObjects(2, arr, false, INFINITE);
	if (r == 1)
	{
		return;
	}
	//cout << "waiting empty finished" << endl;
	int slot = nextSeq % w; int s = ((Packet*)buffer)->size; int t = ((Packet*)buffer)->type;
	memcpy(queue + slot * (MAX_PKT_SIZE + sizeof(Packet)), buffer, s + sizeof(Packet));
	nextSeq++; dataTxed += (s - sizeof(SenderDataHeader));
	ReleaseSemaphore(param.full, 1, NULL);
	pkts += 1;

	//WaitForSingleObject(param.mutex, INFINITE);
	//cout << "Packet " << it << " put into queue, current pkts in queue: " << pkts << endl;
	//ReleaseMutex(param.mutex);
	return;
}

void SenderSocket::tranPacket(int num)
{
	//cout << "Packet: " << num << endl;
	char* p = queue + num * (MAX_PKT_SIZE + sizeof(Packet));
	int pktSize = *(int*)(p + 8);
	int q = *(int*)(p + 16);
	char* sendbuf = new char[pktSize];
	memcpy(sendbuf, p + sizeof(Packet), pktSize);

	sampStart[num] = (DWORD)GetTickCount64();//record start time to calulate sampleRTT later
	if (sendto(sock, (const char*)sendbuf, pktSize, 0, (struct sockaddr*)&remote, sizeof(remote)) == SOCKET_ERROR)
	{
		printf("sendto error with code %d\n", WSAGetLastError());
		WSACleanup();
		return; // sendto() failed
	}
	//WaitForSingleObject(param.mutex, INFINITE);
	//cout << "Packet " << q << " transmitted, current pkts in queue: " << pkts << endl;
	//ReleaseMutex(param.mutex);
	delete sendbuf;
	sendbuf = NULL;
	return;
}

void errorCheck()
{
	printf(" WSAGetLastError code: %d", WSAGetLastError());
	exit(EXIT_FAILURE);
}

void Worker(LPVOID pParam)
{
	WSADATA wsaData;
	WORD wVersionRequested = MAKEWORD(2, 2);
	if (WSAStartup(wVersionRequested, &wsaData) != 0) {
		printf("WSAStartup error %d\n", WSAGetLastError());
		WSACleanup();
		exit(EXIT_FAILURE);
	}
	SenderSocket* ss = (SenderSocket*)pParam; 
	HANDLE socketReceiveReady = CreateEvent(NULL, false, false, NULL);
	WSAEventSelect(ss->sock, socketReceiveReady, FD_READ);
	HANDLE events[] = { socketReceiveReady, ss->param.full };
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	DWORD timerStart = GetTickCount64(), timerExpire = INFINITE;
	char* recvbuf; recvbuf = new char[12];
	double timeout; int ret; int nextToSend = 0;
	//cout << "rtt: " << ss->RTT << endl;
	//cout << "fl: " << ss->fl << endl;
	//cout << "bn: " << ss->bn << endl;
	int w = ss->w; int retranNum = 0; bool workerFlag = false;
	int retxSeq = -1; int fastRetxCount = 0;
	timeval tp;
	while (true)
	{
		if (workerFlag)
		{
			ss->transTime = (double)(GetTickCount64() - ss->transStart) / 1000;
			break;
		}
			
		if (ss->nextSeq != ss->sndBase)
			timeout = timerExpire - GetTickCount64();
		else
			timeout = INFINITE;
		if (ss->rto <= 1)
		{
			tp.tv_sec = 0;
			tp.tv_usec = ss->rto * 1e6;
		}
		else
		{
			tp.tv_sec = ss->rto;
			tp.tv_usec = 0;
		}
		//cout << "waiting" << endl;
		ret = WaitForMultipleObjects(2, events, false, timeout);
		//cout << "waiting result: " << ret << endl;
		switch (ret)
		{
			case WAIT_TIMEOUT:
			{
				if (retranNum == 50)
				{
					printf("Wait timeout...");
					SetEvent(ss->param.eventQuit);
					errorCheck();
				}
				if (retxSeq == ss->sndBase)
					retranNum++;
				else
				{
					retranNum = 1;
					retxSeq = ss->sndBase;
				}

				ss->timeoutNum++;
				//printf("retransmitting packet %d\n", ss->sndBase);
				ss->tranPacket(ss->sndBase % w);
				timerExpire = GetTickCount64() + ss->rto * 1000;
				break;
			}

			case WAIT_OBJECT_0:
			{
				int remoteLen = sizeof(ss->remote);
				fd_set fd;
				FD_ZERO(&fd);
				FD_SET(ss->sock, &fd);
				int available = select(0, &fd, NULL, NULL, &tp);
				if (available > 0)
				{
					int recvL = recvfrom(ss->sock, (char*)recvbuf, 12, 0, (struct sockaddr*)&(ss->remote), &remoteLen);
					if (recvL == SOCKET_ERROR)
					{
						printf("receiving error,");
						errorCheck();
					}
					ReceiverHeader* rh = (ReceiverHeader*)recvbuf;
					//WaitForSingleObject(ss->param.mutex, INFINITE);
					//printf("recv ack of data %d, sendbase %d, estRTT %.3f, RTO %.3f\n", rh->ackSeq, ss->sndBase, ss->estRTT, ss->rto);
					//ReleaseMutex(ss->param.mutex);
					if (rh->ackSeq == ss->finalSeq)
					{
						//printf("All packets ACKed!\n");
						SetEvent(ss->param.eventQuit);
						workerFlag = true;
						break;
					}
					if (rh->ackSeq > ss->sndBase)
					{
						if (retranNum == 0)
						{
							//WaitForSingleObject(ss->param.mutex, INFINITE);
							//cout << "Modifying RTO accroding to " << rh->ackSeq << ' ' << ss->sndBase << endl;
							//ReleaseMutex(ss->param.mutex);
							double sampleRTT = (double)(GetTickCount64() - ss->sampStart[((rh->ackSeq) - 1)%w]) / 1000;
							ss->sumRTT += sampleRTT; ss->RTTCount++;
							ss->estRTT = 7 * ss->estRTT / 8 + sampleRTT / 8;
							ss->devRTT = 3 * ss->devRTT / 4 + fabs(ss->estRTT - ss->devRTT) / 4;
							ss->rto = ss->estRTT + 4 * max(ss->devRTT, 0.01);
							if (ss->estRTT < ss->transRTT)
								ss->transRTT = ss->estRTT;
						}

						ss->sndBase = rh->ackSeq;
						ss->effectiveWin = min(ss->w, rh->recvWnd);
						int newReleased = (ss->sndBase) + ss->effectiveWin - (ss->lastReleased);
						ReleaseSemaphore(ss->param.empty, newReleased, NULL);
						ss->lastReleased += newReleased; ss->pkts -= ss->pkts>newReleased?newReleased:ss->pkts;
						//WaitForSingleObject(ss->param.mutex, INFINITE);
						//cout << "Window moving to " << ss->sndBase << ", current pkts in queue: " << ss->pkts << endl;
						//ReleaseMutex(ss->param.mutex);
						timerExpire = GetTickCount64() + ss->rto * 1000;
						retranNum = 0;
						fastRetxCount = 0;
					}
					else if (rh->ackSeq == ss->sndBase)
					{
						//WaitForSingleObject(ss->param.mutex, INFINITE);
						//printf("recv ack of data %d, sendbase %d, estRTT %.3f, RTO %.3f\n", rh->ackSeq, ss->sndBase, ss->estRTT, ss->rto);
						//ReleaseMutex(ss->param.mutex);
						//printf("recv ack of data %d, window %d, sendbase %d, estRTT %.3f, RTO %.3f\n", rh->ackSeq, ss->effectiveWin, ss->sndBase, ss->estRTT, ss->rto);
						fastRetxCount++;
						if (fastRetxCount == 3)
						{
							if (retxSeq != ss->sndBase)
							{
								//WaitForSingleObject(ss->param.mutex, INFINITE);
								//printf("recv ack of data %d, sendbase %d, estRTT %.3f, RTO %.3f\n", rh->ackSeq, ss->sndBase, ss->estRTT, ss->rto);
								//ReleaseMutex(ss->param.mutex);
								retxSeq = ss->sndBase;
								retranNum = 0;
							}
							//int retxSeq;
							//char* qwe = ss->queue + (ss->sndBase) * (MAX_PKT_SIZE + sizeof(Packet));
							//memcpy(&retxSeq, qwe, 4);
							ss->fastRetxNum++;
							//printf("fast-retransmitting packet %d, ACK counter %d, response seq %d\n", ss->sndBase, fastRetxCount, rh->ackSeq);
							retranNum++;
							ss->tranPacket(ss->sndBase % w);
							timerExpire = GetTickCount64() + ss->rto * 1000;
						}
					}
				}
				break;
			}

			case WAIT_OBJECT_0 + 1:
			{
				//cout << "transmission started" << endl;
				//cout << "window: " << w << endl;
				if (ss->dataFlag)
					ss->finalSeq = ss->nextSeq;
				//cout << "sending: " << nextToSend << endl;
				ss->tranPacket(nextToSend % ss->w);

				//cout << "transmission finished" << endl;
				if (ss->nextSeq != ss->sndBase)
					timerExpire = GetTickCount64() + ss->rto * 1000;
				nextToSend++;
				//WaitForSingleObject(ss->param.mutex, INFINITE);
				//printf("sent packet %d\n", nextToSend);
				//ReleaseMutex(ss->param.mutex);
				break;
			}

			default:
			{
				printf("Wait failed, Error %d\n", GetLastError());
				exit(EXIT_FAILURE);
			}
		}
	}
	SetEvent(ss->param.workerQuit);
	return;
}

void Stats(LPVOID pParam)
{
	SenderSocket* ss = (SenderSocket*)pParam;
	while (WaitForSingleObject(ss->param.eventQuit, 2000) == WAIT_TIMEOUT)
	{
		double statTime = (double)((unsigned long int)GetTickCount64() - ss->timeStart) / 1000;
		printf("[%d]  B %7d  (  %.2f MB) N %7d ", (int)statTime, ss->sndBase, (double)ss->dataTxed / 1e6, ss->nextSeq);
		printf("T %2d F %2d W %6d S %.3f Mbps RTT %.3f RTO %.3f\n", ss->timeoutNum, ss->fastRetxNum, ss->effectiveWin, (double)(ss->dataTxed) * 8 / (1e6 * statTime), ss->estRTT, ss->rto);
	}
}

void SenderSocket::test()
{
	printf("This is a test function\n");
	return;
}

SenderSocket::~SenderSocket()
{
	WaitForSingleObject(ths[0], INFINITE);
	WaitForSingleObject(ths[1], INFINITE);
	//printf("All threads exited!\n");
}

