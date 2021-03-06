#include <pthread.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <fcntl.h>
#include <errno.h>
#include <map>
#include <utils/Log.h>
#include <rpc/Control.h>
#include <rpc/thread_pool.h>
#include <rpc/share_rpc.h>
#include "time.h" 

namespace android {
// ---------------------------------------------------------------------------

/* <linux/tcp.h> was giving me a problem on some systems.  Copied below are the
 * definitions we need from it. */
#define TCP_NODELAY   1 /* Turn off Nagle's algorithm. */
#define TCP_CORK    3 /* Never send partially complete segments */

#define RTT_INFINITE 10*1000*1000 // 10 seconds

#define CONTROL_DIAGNOSTIC_PORT_S "5554"
#define CONTROL_TRANSPORT_PORT_S "5555"

#define MAX_CONTROL_VPACKET_SIZE (1<<16)

// TODO: Move compression only into tcpmux layer.
//#define USE_COMPRESSION

#ifdef USE_COMPRESSION
#include "zlib.h"
#define Z_LEVEL Z_DEFAULT_COMPRESSION
#endif

#define SETOPT(s, opt, val) do { \
    int v = (val); \
    if(setsockopt((s), IPPROTO_TCP, (opt), (void*)&v, sizeof(int))) { \
      perror("setsockopt"); /* It should be ok. */ \
      abort(); \
    } \
  } while(0)

#define CHECK_RESULT(_str, _res)                                            \
    if((_res) == -1) {                                                      \
      /*ALOGW("Unexpected failure with %s (%s)", (_str), strerror(errno));*/     \
      std::cout<<"Unexpected failure with " << _str << strerror(errno); \
      break;                                                                \
    } \
    
    /* else if((_res) == 0) {                                                \
      break;                                                                \
    }*/

int rpcNetPipe[2];

// total number of bytes actually read
u8 totalReadBytes = 0;
// total number of bytes actually write
u8 totalWriteBytes = 0;
// total number of bytes shall read if without compression
u8 totalOReadBytes = 0;
// total number of bytes shall write if without compression
u8 totalOWriteBytes = 0;

typedef struct MsgHeader {
    u1 type;
    u4 seqNo;
    union {
        struct {
            u4 serviceId;
            u4 methodId;
        } req;
        struct {
            int errorNo;
            int pad;
        } res;
    };
    u4 totalSz;
    u4 sz;
} MsgHeader;

typedef struct ReadInd {
    int rst; 
    u4 rsz; 
    u4 rpos;
    MsgHeader rhdr;
    char rbuf[2*MAX_CONTROL_VPACKET_SIZE];
#ifdef USE_COMPRESSION
    char rbuftmp[2*MAX_CONTROL_VPACKET_SIZE];
#endif
    
    ReadInd() : rst(0), rsz(sizeof(MsgHeader)), rpos(0) {}
} ReadInd;

typedef struct WriteInd {
    // flags to indicate if the write is for the header or the data content
    int wst; 
    // indicate that how many byte should be written for the next write
    u4 wsz; 
    // the position of the current write
    u4 wpos;
    // the original data size before compression
    u4 owsz;
    MsgHeader whdr;
    char* wbuf;
#ifdef USE_COMPRESSION
    char wtmpbuf[2*MAX_CONTROL_VPACKET_SIZE];
#endif
    
    WriteInd() : wst(-1), wsz(sizeof(MsgHeader)), wpos(0), owsz(-1) {}
} WriteInd;

/** Lock for retrieving the next response id */
pthread_mutex_t outRpcLock;

/** condition for rpc to recieve response */
std::map<u8, pthread_mutex_t*> idxMtxMaps;
std::map<u8, pthread_cond_t*> idxCondMaps;
std::vector<pthread_mutex_t*> mtxPool;
std::vector<pthread_cond_t*> condPool;
//pthread_cond_t rpcResCond;
pthread_mutex_t* rpcResLock;

/** A queue to store the list of rpc requests */
std::map<int, std::queue<RpcMessage*> > outRpcMsgs;

/** A map to receive all the data for a rpc message */
std::map<u8, RpcMessage*> inMsgMaps;

/** arrived rpc responses */
std::map<u8, RpcResponse*> arrivedResps;

/** the ind flag showing which data should expected from a connection */
std::map<int, ReadInd*> readInds;

/** the ind flag showing which data should write to a connection */
std::map<int, WriteInd*> writeInds;

/** thread pool for rpc invocation */
static ThreadPool* rpcThdPool;

void message_loop(RpcEndpoint* endpoint) {
    //ALOGI("Starting message loop");

    int res;

#ifdef USE_COMPRESSION
    z_stream wstrm;
    wstrm.zalloc = Z_NULL; wstrm.zfree = Z_NULL; wstrm.opaque = Z_NULL;
    deflateInit(&wstrm, Z_LEVEL);

    z_stream rstrm;
    rstrm.zalloc = Z_NULL; rstrm.zfree = Z_NULL; rstrm.opaque = Z_NULL;
    inflateInit(&rstrm);
#endif

    // These four variables are for debugging purposes.
    /*long long read_bytes = 0;
    long long read_acked_bytes = 0;
    long long sent_bytes = 0;
    long long sent_acked_bytes = 0;
    long long cread_bytes = 0;
    long long cread_acked_bytes = 0;
    long long csent_bytes = 0;
    long long csent_acked_bytes = 0;*/

    while(1) {
        std::vector<int> wsfds;
        for(std::map<int, std::queue<RpcMessage*> >::iterator it = outRpcMsgs.begin(); it != outRpcMsgs.end(); ++it) {
            WriteInd* wind = writeInds[it->first];
            if(!it->second.empty()) {
                wsfds.push_back(it->first);
            }
            if(wind->wst == -1 && !it->second.empty()) {
                RpcMessage* rpcMsg = it->second.front();
                wind->wst = 0;
                wind->wsz = sizeof(MsgHeader);
                wind->wpos = 0;
                wind->whdr.type = rpcMsg->type;
                wind->whdr.seqNo = htonl(rpcMsg->seqNo);
                if(rpcMsg->type == RpcMessage::MSG_TYPE_REQUEST) {
                    RpcRequest* rpcReq = static_cast<RpcRequest*> (rpcMsg);
                    wind->whdr.req.serviceId = htonl(rpcReq->serviceId);
                    wind->whdr.req.methodId = htonl(rpcReq->methodId);
                    wind->whdr.totalSz = htonl(rpcReq->argsSize);
                    wind->whdr.sz = fifoGetBufferSize(rpcReq->args);
                    wind->wbuf = fifoGetBuffer(rpcReq->args);
                } else {
                    RpcResponse* rpcRes = static_cast<RpcResponse*> (rpcMsg);
                    wind->whdr.res.errorNo = htonl(rpcRes->errorNo);
                    wind->whdr.totalSz = htonl(rpcRes->retSize);
                    wind->whdr.sz = fifoGetBufferSize(rpcRes->ret);
                    wind->wbuf = fifoGetBuffer(rpcRes->ret);
                }

                wind->whdr.sz = htonl(wind->whdr.sz < MAX_CONTROL_VPACKET_SIZE ?
                                wind->whdr.sz : MAX_CONTROL_VPACKET_SIZE);
                wind->owsz = ntohl(wind->whdr.sz);

                //sent_bytes += ntohl(whdr.sz);
#ifdef USE_COMPRESSION
                /* Perform the compression with zlib. */
                if (ntohl(wind->whdr.sz) != 0) {
                    COMPRESSION_PROFILING_START()
                    wstrm.avail_in = ntohl(wind->whdr.sz);
                    wstrm.next_in = (unsigned char*)wind->wbuf;
                    wstrm.avail_out = sizeof(wind->wtmpbuf);
                    wstrm.next_out = (unsigned char*)(wind->wtmpbuf);
                    //ALOGE("rpc service control start to deflate, %d - %d - %d", ntohl(wind->whdr.sz), wind->whdr.seqNo, ntohl(wind->whdr.totalSz));
                    deflate(&wstrm, Z_SYNC_FLUSH);
                    //ALOGE("rpc service control after deflating, %d - %d - %d", ntohl(wind->whdr.sz), sizeof(wind->wtmpbuf) - wstrm.avail_out, wstrm.avail_in);
                    wind->wbuf = wind->wtmpbuf;
                    wind->whdr.sz = htonl(sizeof(wind->wtmpbuf) - wstrm.avail_out);
                    COMPRESSION_PROFILING_END(ntohl(wind->whdr.seqNo), "compression")
                }
#endif
                //csent_bytes += ntohl(whdr.sz);
            }/* else if(wst == -1 && sent_bytes != sent_acked_bytes) {
                //ALOGI("WRITE[b, db, cb, dcb] = [%lld, %lld, %lld, %lld]",
                //     sent_bytes, sent_bytes - sent_acked_bytes,z
                //     csent_bytes, csent_bytes - csent_acked_bytes);
                sent_acked_bytes = sent_bytes;
                csent_acked_bytes = csent_bytes;
            }*/
        }

        
        fd_set rdst; FD_ZERO(&rdst);
        fd_set wrst; FD_ZERO(&wrst);
        int nfd = rpcNetPipe[0] + 1;
        FD_SET(rpcNetPipe[0], &rdst);
        // look for max socket fd
        for(std::map<int, ReadInd*>::iterator it = readInds.begin(); it != readInds.end(); ++it) {
            if(it->first + 1 > nfd) {
                nfd = it->first + 1;
            }
            FD_SET(it->first, &rdst);   // to check if there is any data from the other endpoint
        }
        for(unsigned int i = 0; i < wsfds.size(); i++) {
            FD_SET(wsfds.at(i), &wrst);   // to check if it is able to write to the other endpoint
        }
        res = select(nfd, &rdst, &wrst, NULL, NULL);
        CHECK_RESULT("select", res);

        for(std::map<int, ReadInd*>::iterator it = readInds.begin(); it != readInds.end(); ++it) {
            int sockFd = it->first;
            if(FD_ISSET(sockFd, &rdst)) {
                ReadInd* rind = readInds[sockFd];
                if(rind->rst == 0) { // indicating that it is now reading message header
                    res = read(sockFd, ((char*)&rind->rhdr) + rind->rpos, rind->rsz - rind->rpos);
                    CHECK_RESULT("read", res);
                    // FIXME: this might be wrong. but when there is a bug when the remote end is closed by exception, then this end will always return as be able to read and cause dead loop
                    if (res == 0) {
                        close(sockFd);
                        delete readInds[sockFd];
                        readInds.erase(sockFd);
                        continue;
                    }
                    rind->rpos += res;
                    totalReadBytes += res;
                    totalOReadBytes += res;

                    if(rind->rpos == rind->rsz) {
                        rind->rhdr.type = rind->rhdr.type;
                        rind->rhdr.sz = ntohl(rind->rhdr.sz);
                        rind->rhdr.seqNo = ntohl(rind->rhdr.seqNo);
                        rind->rhdr.totalSz = ntohl(rind->rhdr.totalSz);
                        if(rind->rhdr.type == RpcMessage::MSG_TYPE_REQUEST) {
                            rind->rhdr.req.serviceId = ntohl(rind->rhdr.req.serviceId);
                            rind->rhdr.req.methodId = ntohl(rind->rhdr.req.methodId);
                        } else {
                            rind->rhdr.res.errorNo = ntohl(rind->rhdr.res.errorNo);
                        }
                        rind->rst = 1;
                        rind->rpos = 0;
                        rind->rsz = rind->rhdr.sz;

                        if(rind->rsz > MAX_CONTROL_VPACKET_SIZE) {
                            //ALOGE("Invalid message size %d", rind->rsz);
                            abort();
                        }
                        
                        // no further data contents for this read
                        if(rind->rsz == 0) {
                            if(rind->rhdr.type == RpcMessage::MSG_TYPE_REQUEST) {
                                RpcRequest* rpcReq = new RpcRequest();
                                rpcReq->type = rind->rhdr.type;
                                rpcReq->seqNo = rind->rhdr.seqNo;
                                rpcReq->socketFd = sockFd;
                                rpcReq->serviceId = rind->rhdr.req.serviceId;
                                rpcReq->methodId = rind->rhdr.req.methodId;
                                rpcReq->argsSize = rind->rhdr.totalSz;
                                    //ALOGE("rpc audio service request has been received");
                                
                                // if have recieved all the data, put to a working thread
                                //SERVER_RPC_PROFILING_START(rpcReq->seqNo)
                                RpcWorkerThread* workerThd = new RpcWorkerThread(endpoint, rpcReq);
                                rpcThdPool->assignWork(workerThd);
                            } else {
                                RpcResponse* rpcRes = new RpcResponse();
                                u8 idxId = sockFd;
                                idxId <<= 4;
                                idxId += rind->rhdr.seqNo;
                                rpcRes->type = rind->rhdr.type;
                                rpcRes->seqNo = rind->rhdr.seqNo;
                                rpcRes->errorNo = rind->rhdr.res.errorNo;
                                rpcRes->retSize = rind->rhdr.totalSz;
                                    //ALOGE("rpc audio service response has been received");
                                     
                                pthread_mutex_lock(idxMtxMaps[idxId]); 
                                arrivedResps[idxId] = rpcRes;
                                // notify the waiting rpc threads
                                //ALOGE("rpc audio service the control signal the idx: %lld, sockfd: %d, seq: %d, mtx: %d, cond: %d", idxId, sockFd, rind->rhdr.seqNo, idxMtxMaps[idxId], idxCondMaps[idxId]);
                                pthread_cond_signal(idxCondMaps[idxId]);
                                //CLIENT_REMOTE_PROFILING_END(rpcRes->seqNo)
                                pthread_mutex_unlock(idxMtxMaps[idxId]);
                            }

                            rind->rst = 0;
                            rind->rpos = 0;
                            rind->rsz = sizeof(MsgHeader);
                        }
                    }
                } else if(rind->rst == 1) { // indicating that it is now reading the rpc message contents
                    res = read(sockFd, rind->rbuf + rind->rpos, rind->rsz - rind->rpos);
                    CHECK_RESULT("read", res);
                    rind->rpos += res;
                    totalReadBytes += res;

                    if(rind->rpos == rind->rsz) {
#ifdef USE_COMPRESSION
                        /* Do the decompression and push the data to the thread. */
                        COMPRESSION_PROFILING_START()
                        rstrm.avail_in = rind->rsz;
                        rstrm.next_in = (unsigned char*)rind->rbuf;
                        rstrm.avail_out = sizeof(rind->rbuftmp);
                        rstrm.next_out = (unsigned char*)rind->rbuftmp;
                        int ret = inflate(&rstrm, Z_SYNC_FLUSH);
                        totalOReadBytes += sizeof(rind->rbuftmp) - rstrm.avail_out;
                        COMPRESSION_PROFILING_END(rind->rhdr.seqNo, "decompression")
                        //ALOGE("rpc service control the inflate size: %d - %d", rstrm.avail_out, ret);
#else
                        totalOReadBytes += rind->rsz;
#endif

#ifdef USE_COMPRESSION
                        if(rstrm.avail_out != sizeof(rind->rbuftmp)) {
#else
                        if(rind->rsz != 0) {
#endif
                            if(rind->rhdr.type == RpcMessage::MSG_TYPE_REQUEST) {
                                RpcRequest* rpcReq;
                                u8 idxId = sockFd;
                                idxId <<= 4;
                                idxId += rind->rhdr.seqNo;
                                if(inMsgMaps.find(idxId) == inMsgMaps.end()) {
                                    rpcReq = new RpcRequest();
                                    rpcReq->type = rind->rhdr.type;
                                    rpcReq->seqNo = rind->rhdr.seqNo;
                                    rpcReq->socketFd = sockFd;
                                    rpcReq->serviceId = rind->rhdr.req.serviceId;
                                    rpcReq->methodId = rind->rhdr.req.methodId;
                                    rpcReq->argsSize = rind->rhdr.totalSz;
                                    rpcReq->args = fifoCreate();
                                    inMsgMaps[idxId] = rpcReq;
                                } else {
                                    rpcReq = static_cast<RpcRequest*> (inMsgMaps[idxId]);
                                }
#ifdef USE_COMPRESSION
                                fifoPushData(rpcReq->args, rind->rbuftmp, sizeof(rind->rbuftmp) - rstrm.avail_out);
#else
                                fifoPushData(rpcReq->args, rind->rbuf, rind->rsz);
#endif
                                // if have recieved all the data, put to a working thread
                                if(rpcReq->argsSize == fifoSize(rpcReq->args)) {
                                    inMsgMaps.erase(idxId);
                                //ALOGE("rpc audio service request has been received");
                                    //SERVER_RPC_PROFILING_START(rpcReq->seqNo)
                                    RpcWorkerThread* workerThd = new RpcWorkerThread(endpoint, rpcReq);
                                    rpcThdPool->assignWork(workerThd);
                                }
                            } else {
                                RpcResponse* rpcRes;
                                u8 idxId = sockFd;
                                idxId <<= 4;
                                idxId += rind->rhdr.seqNo;
                                if(inMsgMaps.find(idxId) == inMsgMaps.end()) {
                                    rpcRes = new RpcResponse();
                                    rpcRes->type = rind->rhdr.type;
                                    rpcRes->seqNo = rind->rhdr.seqNo;
                                    rpcRes->errorNo = rind->rhdr.res.errorNo;
                                    rpcRes->retSize = rind->rhdr.totalSz;
                                    rpcRes->ret = fifoCreate();
                                    inMsgMaps[idxId]= rpcRes;
                                } else {
                                    rpcRes = static_cast<RpcResponse*> (inMsgMaps[idxId]);
                                }
#ifdef USE_COMPRESSION
                                fifoPushData(rpcRes->ret, rind->rbuftmp, sizeof(rind->rbuftmp) - rstrm.avail_out);
#else
                                fifoPushData(rpcRes->ret, rind->rbuf, rind->rsz);
#endif
                                //if have received all the data, awake a waiting rpc thread
                                if(rpcRes->retSize == fifoSize(rpcRes->ret)) {
                                    pthread_mutex_lock(idxMtxMaps[idxId]); 
                                    arrivedResps[idxId] = rpcRes;
                                    inMsgMaps.erase(idxId);
                                //ALOGE("rpc audio service response has been received");
                                    // notify the waiting rpc threads
                            //ALOGE("rpc audio service the control signal the idx: %lld, sockfd: %d, seq: %d, mtx: %d, cond: %d", idxId, sockFd, rind->rhdr.seqNo, idxMtxMaps[idxId], idxCondMaps[idxId]);
                                    pthread_cond_signal(idxCondMaps[idxId]);
                                    //CLIENT_REMOTE_PROFILING_END(rpcRes->seqNo)
                                    pthread_mutex_unlock(idxMtxMaps[idxId]);
                                }
                            }
                        }

                        rind->rst = 0;
                        rind->rpos = 0;
                        rind->rsz = sizeof(MsgHeader);
                    }
                }
            }
        }
        for(unsigned int i = 0; i < wsfds.size(); i++) {
            int s = wsfds.at(i);
            if(FD_ISSET(s, &wrst)) {
                WriteInd* wind = writeInds[s];
                if(wind->wst == 0) {  // indicating the start of writing header
                    res = write(s, ((char*)&wind->whdr) + wind->wpos, wind->wsz - wind->wpos);
                    CHECK_RESULT("write", res);
                    wind->wpos += res;
                    totalWriteBytes += res;
                    totalOWriteBytes += res;

                    if(wind->wpos == wind->wsz) {
                        wind->wsz = ntohl(wind->whdr.sz);
                        wind->wpos = 0;
                        wind->wst = 1;
                    }
                } else if(wind->wst == 1) { // indicating the start of writing actual content data
                    res = write(s, wind->wbuf, wind->wsz - wind->wpos);
                    CHECK_RESULT("write", res);
                    wind->wbuf += res;
                    wind->wpos += res;
                    totalWriteBytes += res;

                    if(wind->wpos == wind->wsz) { // indicating that all the data has already been written
                        totalOWriteBytes += wind->owsz;
                        wind->wst = -1;
                        
                        RpcMessage* rpcMsg = outRpcMsgs[s].front();
                        if(rpcMsg->type == RpcMessage::MSG_TYPE_REQUEST) {
                            RpcRequest* rpcReq = static_cast<RpcRequest*> (rpcMsg);
                            fifoPopBytes(rpcReq->args, wind->owsz);
                            if(fifoEmpty(rpcReq->args)) {
                                pthread_mutex_lock(&outRpcLock); {
                                    outRpcMsgs[s].pop();
                                } pthread_mutex_unlock(&outRpcLock);
                                CLIENT_REMOTE_PROFILING_START(rpcReq->seqNo, rpcReq->serviceId, rpcReq->methodId);
                                //ALOGE("rpc audio service request has been sent");
                                delete rpcReq;
                            }
                        } else {
                            RpcResponse* rpcRes = static_cast<RpcResponse*> (rpcMsg);
                            fifoPopBytes(rpcRes->ret, wind->owsz);
                            if(fifoEmpty(rpcRes->ret)) {
                                pthread_mutex_lock(&outRpcLock); {
                                    outRpcMsgs[s].pop();
                                } pthread_mutex_unlock(&outRpcLock);
                                SERVER_RPC_PROFILING_END(rpcRes->seqNo)
                                //ALOGE("rpc audio service response has been sent");
                                delete rpcRes;
                            }
                        }

                        if(outRpcMsgs[s].empty()) {// wthread->offCorkLevel == 0) {
                            /* We have nothing more to send right now.  Let any partial packets go over the wire now. */
                            SETOPT(s, TCP_NODELAY, 1);
                            SETOPT(s, TCP_NODELAY, 0);
                        }
                    }
                }
            }
        }
        if(FD_ISSET(rpcNetPipe[0], &rdst)) {
            // read the data out, or the pipe will be always signaled to have data to read, which cause a dead loop
            int dummySockFd;
            read(rpcNetPipe[0], &dummySockFd, sizeof(dummySockFd));
        }
        wsfds.clear();
    }

    for(std::map<int, std::queue<RpcMessage*> >::iterator it = outRpcMsgs.begin(); it != outRpcMsgs.end(); ++it) {
        std::queue<RpcMessage*> emptyQ;
        std::swap(it->second, emptyQ);
    }
    outRpcMsgs.clear();
    rpcThdPool->destroyPool(2);
    delete rpcThdPool;
}

void addOutRpcMsg(RpcMessage* rpcMsg) {
    pthread_mutex_lock(&outRpcLock); {
        outRpcMsgs[rpcMsg->socketFd].push(rpcMsg);
    } pthread_mutex_unlock(&outRpcLock);
    
    // notify there is something to write to jump out select(), via a pipe
    write(rpcNetPipe[1], &rpcMsg->socketFd, sizeof(rpcMsg->socketFd));
}

void setupConnection(int socketFd) {
    // add socket fd to writeInds and readInds
    writeInds[socketFd] = new WriteInd();
    readInds[socketFd] = new ReadInd();
    
    // set tcp attribute
    SETOPT(socketFd, TCP_CORK, 1);
    
    // write to jump out select(), via a pipe, so that it can 
    write(rpcNetPipe[1], &socketFd, sizeof(socketFd));
    
    // todo: see if we need to initiate outRpcMsgs with this socket fd
}

void acquireLock(u8 idxId)
{
    pthread_mutex_lock(rpcResLock);
    if (mtxPool.empty()) {
        pthread_mutex_t* mutex = new pthread_mutex_t();
        pthread_mutex_init(mutex, NULL);
        mtxPool.push_back(mutex);
        pthread_cond_t* cond = new pthread_cond_t();
        pthread_cond_init(cond, NULL);
        condPool.push_back(cond);
    }
    pthread_mutex_t* mtx = mtxPool.back();
    mtxPool.pop_back();
    pthread_cond_t* cond = condPool.back();
    condPool.pop_back();
    idxMtxMaps[idxId] = mtx;
    idxCondMaps[idxId] = cond;
    pthread_mutex_unlock(rpcResLock);
}

void releaseLock(u8 idxId)
{
    pthread_mutex_lock(rpcResLock);
    pthread_mutex_t* mtx = idxMtxMaps[idxId];
    mtxPool.push_back(mtx);
    pthread_cond_t* cond = idxCondMaps[idxId];
    condPool.push_back(cond);
    idxMtxMaps.erase(idxId);
    idxCondMaps.erase(idxId);
    pthread_mutex_unlock(rpcResLock);
}

/* create a thread to profiling how many bytes are sent and read in every minite */
static void* dataProfiling(void* args)
{
    u8 lastTotalReadBytes = 0;
    u8 lastTotalWriteBytes = 0;
    u8 lastTotalOReadBytes = 0;
    u8 lastTotalOWriteBytes = 0;
    while (true) {
        sleep(60);
        ALOGE("[rpc evaluation], data throughput in bytes for last minute, totalRead: %lld, totalWrite: %lld, totalShallRead: %lld, totalShallWrite: %lld", (totalReadBytes - lastTotalReadBytes), (totalWriteBytes - lastTotalWriteBytes), (totalOReadBytes - lastTotalOReadBytes), (totalOWriteBytes - lastTotalOWriteBytes));
        lastTotalReadBytes = totalReadBytes;
        lastTotalWriteBytes = totalWriteBytes;
        lastTotalOReadBytes = totalOReadBytes;
        lastTotalOWriteBytes = totalOWriteBytes;
    }
    
    return NULL;
}

void controlInit() {
    int poolSize = 4;
    rpcThdPool = new ThreadPool(poolSize);
    rpcThdPool->initializeThreads();
    
    pthread_mutex_init(&outRpcLock, NULL);
    rpcResLock = new pthread_mutex_t();
    pthread_mutex_init(rpcResLock, NULL);
    
    pipe(rpcNetPipe);
    
    
    pthread_t dataProfilingThread;
    pthread_create(&dataProfilingThread, NULL, dataProfiling, NULL);
}

// ---------------------------------------------------------------------------
}; // namespace android
