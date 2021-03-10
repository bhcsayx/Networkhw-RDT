#include <iostream>
#include <winsock2.h>
#include "utils.h"
#include "SenderSocket.h"
#include "Checksum.h"

#define MAX_PKT_SIZE (1500-28)
#pragma comment(lib, "ws2_32.lib")
using namespace std;


int main(int argc, char* argv[])
{
    char* targetHost = new char[strlen(argv[1]) + 1];
    strcpy_s(targetHost, strlen(argv[1]) + 1, argv[1]);
    int power = atoi(argv[2]);
    int window = atoi(argv[3]);
    double RTT = atof(argv[4]);
    double forLoss = atof(argv[5]);
    double retLoss = atof(argv[6]);
    double bottleNeck = atof(argv[7]);

    cout << "Main: sender W = " << window << ", RTT " << RTT << " sec, loss " << forLoss << " / ";
    cout << retLoss << ", link " << bottleNeck << " Mbps" << endl;
    
    SenderSocket soc(targetHost, window, RTT, forLoss, retLoss, bottleNeck);
    soc.Open();
    soc.ths[0] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Worker, (void*)&soc, 0, NULL);
    soc.ths[1] = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)Stats, (void*)&soc, 0, NULL);
    printf("Main: connected to %s in %.3f sec, packet size 1472 bytes\n", targetHost, (double)(GetTickCount64() - soc.timeStart) / 1000);
    uint64_t dwordBufSize = (uint64_t)1 << power;
    DWORD* dwordBuf = new DWORD[dwordBufSize];
    for (uint64_t i = 0; i < dwordBufSize; i++) // required initialization
        dwordBuf[i] = i;
    cout << "Main: initializing DWORD array with 2^" << power << " elements... done in " << GetTickCount64() - soc.timeStart << " ms" << endl;
    char* charBuf = (char*)dwordBuf; // this buffer goes into socket
    UINT64 byteBufferSize = dwordBufSize << 2; // convert to bytes
    UINT64 off = 0; // current position in buffer

    while (off < byteBufferSize)
    {
        int bytes = min(byteBufferSize - off, MAX_PKT_SIZE - sizeof(SenderDataHeader));
        //cout << "sending pkt: " << soc.it << ' ' << bytes << endl;
        if (off + bytes >= byteBufferSize)
            soc.dataFlag = true;
        soc.consData(charBuf + off, bytes);
        soc.Send(charBuf + off, bytes);
        off += bytes;
        soc.it += 1;
    }
    
    WaitForSingleObject(soc.param.workerQuit, INFINITE);

    soc.Close();   

    Checksum cs;

    soc.transSpeed = byteBufferSize * 8 / (soc.transTime * 1000);
    //cout << "transRTT: " << soc.transRTT << endl;
    double averRTT = soc.sumRTT / soc.RTTCount;
    printf("Main: transfer finished in %.3f sec, %.3f Kbps, checksum %x\n",soc.transTime, soc.transSpeed, cs.CRC32((unsigned char*)charBuf, byteBufferSize));
    printf("Main: estRTT %.3f, ideal rate %.3f Kbps\n", soc.estRTT, (double)soc.w * 8 * MAX_PKT_SIZE/(soc.estRTT * 1000));
    return 0;
}

