/*
 * nord_usb.c — Nord Electro 6 USB protocol library implementation
 */

#include "nord_usb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>

// ─── CRC-16/CCITT ───

uint16_t nord_crc16(const uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc & 0xFFFF;
}

// ─── Message building ───

static void put_be32(uint8_t *p, uint32_t v) {
    p[0] = (v >> 24) & 0xFF;
    p[1] = (v >> 16) & 0xFF;
    p[2] = (v >> 8) & 0xFF;
    p[3] = v & 0xFF;
}

static uint32_t get_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t get_be16(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | (uint16_t)p[1];
}

int nord_build_message(uint8_t *buf, int bufSize,
                       uint32_t protoId, uint32_t subId, uint32_t msgType,
                       const uint8_t *payload, int payloadLen) {
    int totalSize = NORD_HEADER_SIZE + payloadLen + NORD_CRC_SIZE;
    if (totalSize > bufSize) return -1;

    memset(buf, 0, totalSize);
    put_be32(buf + 0, totalSize);
    put_be32(buf + 4, protoId);
    put_be32(buf + 8, subId);
    put_be32(buf + 12, msgType);

    if (payload && payloadLen > 0)
        memcpy(buf + NORD_HEADER_SIZE, payload, payloadLen);

    uint16_t crc = nord_crc16(buf, totalSize - NORD_CRC_SIZE);
    buf[totalSize - 2] = (crc >> 8) & 0xFF;
    buf[totalSize - 1] = crc & 0xFF;

    return totalSize;
}

bool nord_parse_header(const uint8_t *data, int len,
                       uint32_t *totalSize, uint32_t *protoId,
                       uint32_t *subId, uint32_t *msgType) {
    if (len < NORD_HEADER_SIZE) return false;

    *totalSize = get_be32(data);
    *protoId = get_be32(data + 4);
    *subId = get_be32(data + 8);
    *msgType = get_be32(data + 12);

    // Validate CRC if we have the full message
    if ((int)*totalSize <= len && *totalSize >= NORD_HEADER_SIZE + NORD_CRC_SIZE) {
        uint16_t payloadSize = *totalSize - NORD_CRC_SIZE;
        uint16_t recvCrc = get_be16(data + payloadSize);
        uint16_t calcCrc = nord_crc16(data, payloadSize);
        if (recvCrc != calcCrc) {
            fprintf(stderr, "CRC mismatch: recv=0x%04X calc=0x%04X\n", recvCrc, calcCrc);
            return false;
        }
    }

    return true;
}

void nord_hexdump(const char *label, const uint8_t *data, int len) {
    printf("  %s (%d bytes):", label, len);
    for (int i = 0; i < len && i < 256; i++) {
        if (i % 16 == 0) printf("\n    ");
        printf("%02X ", data[i]);
    }
    if (len > 256) printf("\n    ... (%d more bytes)", len - 256);
    printf("\n");
}

// ─── Async callbacks ───

static void intCallback(void *refCon, IOReturn result, void *arg0) {
    NordDevice *nd = (NordDevice *)refCon;
    UInt32 n = (UInt32)(uintptr_t)arg0;

    if (result == kIOReturnSuccess && n > 0) {
        // Interrupt data received — silently ignore unless debugging
    } else if (result == kIOReturnAborted) {
        return;
    }

    if (nd->intf)
        (*nd->intf)->ReadPipeAsyncTO(nd->intf, nd->intPipe,
            nd->intBuf, NORD_INTERRUPT_BUF, 0, 0, intCallback, nd);
}

static void bulkCallback(void *refCon, IOReturn result, void *arg0) {
    NordDevice *nd = (NordDevice *)refCon;
    UInt32 n = (UInt32)(uintptr_t)arg0;

    if (result == kIOReturnSuccess && n > 0) {
        pthread_mutex_lock(&nd->respMutex);
        memcpy(nd->respBuf, nd->bulkBuf, n > NORD_BULK_BUF ? NORD_BULK_BUF : n);
        nd->respLen = n;
        nd->gotResponse = true;
        pthread_cond_signal(&nd->respCond);
        pthread_mutex_unlock(&nd->respMutex);
    } else if (result == kIOReturnAborted) {
        return;
    }

    if (nd->intf)
        (*nd->intf)->ReadPipeAsyncTO(nd->intf, nd->bulkInPipe,
            nd->bulkBuf, NORD_BULK_BUF, 0, 0, bulkCallback, nd);
}

static void *ioThreadFunc(void *arg) {
    NordDevice *nd = (NordDevice *)arg;
    nd->runLoop = CFRunLoopGetCurrent();

    CFRunLoopSourceRef src;
    (*nd->intf)->CreateInterfaceAsyncEventSource(nd->intf, &src);
    CFRunLoopAddSource(nd->runLoop, src, kCFRunLoopDefaultMode);

    nd->runLoopReady = true;

    (*nd->intf)->ReadPipeAsyncTO(nd->intf, nd->intPipe,
        nd->intBuf, NORD_INTERRUPT_BUF, 0, 0, intCallback, nd);
    (*nd->intf)->ReadPipeAsyncTO(nd->intf, nd->bulkInPipe,
        nd->bulkBuf, NORD_BULK_BUF, 0, 0, bulkCallback, nd);

    CFRunLoopRun();
    return NULL;
}

// ─── Connection lifecycle ───

NordDevice *nord_open(void) {
    NordDevice *nd = calloc(1, sizeof(NordDevice));
    if (!nd) return NULL;

    pthread_mutex_init(&nd->respMutex, NULL);
    pthread_cond_init(&nd->respCond, NULL);

    // Find any Clavia (Nord) device by vendor ID.
    // Try known PIDs first, then fall back to VID-only scan.
    static const int knownPids[] = {
        0x0029,  // Nord Electro 6
        0x002A,  // Nord Piano 4
        0x002B,  // Nord Stage 3 (estimated)
        0x002C,  // Nord Wave 2 (estimated)
    };

    io_service_t svc = 0;

    // Strategy 1: Try each known PID (reliable single-match)
    for (int i = 0; i < (int)(sizeof(knownPids)/sizeof(knownPids[0])); i++) {
        CFMutableDictionaryRef md = IOServiceMatching("IOUSBHostDevice");
        if (!md) md = IOServiceMatching(kIOUSBDeviceClassName);
        if (!md) continue;

        CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &(int){NORD_VID});
        CFNumberRef p = CFNumberCreate(NULL, kCFNumberSInt32Type, &knownPids[i]);
        CFDictionarySetValue(md, CFSTR("idVendor"), v);
        CFDictionarySetValue(md, CFSTR("idProduct"), p);
        CFRelease(v); CFRelease(p);

        svc = IOServiceGetMatchingService(kIOMasterPortDefault, md);
        if (svc) break;
    }

    // Strategy 2: VID-only scan using iterator (catches unknown PIDs)
    if (!svc) {
        CFMutableDictionaryRef md = IOServiceMatching("IOUSBHostDevice");
        if (!md) md = IOServiceMatching(kIOUSBDeviceClassName);
        if (md) {
            CFNumberRef v = CFNumberCreate(NULL, kCFNumberSInt32Type, &(int){NORD_VID});
            CFDictionarySetValue(md, CFSTR("idVendor"), v);
            CFRelease(v);

            io_iterator_t iter;
            if (IOServiceGetMatchingServices(kIOMasterPortDefault, md, &iter) == kIOReturnSuccess) {
                svc = IOIteratorNext(iter);
                IOObjectRelease(iter);
            }
        }
    }

    if (!svc) {
        fprintf(stderr, "No Nord keyboard found (looking for USB VID 0x%04X)\n", NORD_VID);
        free(nd);
        return NULL;
    }

    // Device interface
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    IOCreatePlugInInterfaceForService(svc, kIOUSBDeviceUserClientTypeID,
                                       kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (!plug) { free(nd); return NULL; }

    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID182),
                            (LPVOID*)&nd->dev);
    (*plug)->Release(plug);
    if (!nd->dev) { free(nd); return NULL; }

    // Open device
    IOReturn kr;
    for (int i = 0; i < 12; i++) {
        kr = (*nd->dev)->USBDeviceOpenSeize(nd->dev);
        if (kr == kIOReturnSuccess) break;
        usleep(50000);
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "Failed to open device: 0x%08x\n", kr);
        (*nd->dev)->Release(nd->dev);
        free(nd);
        return NULL;
    }

    // Read product ID
    UInt16 pid = 0;
    (*nd->dev)->GetDeviceProduct(nd->dev, &pid);
    nd->productId = pid;

    // Read product name from USB string descriptor (index 2)
    {
        IOUSBDevRequest strReq;
        uint8_t strBuf[256];
        memset(strBuf, 0, sizeof(strBuf));
        strReq = (IOUSBDevRequest){
            .bmRequestType = 0x80,
            .bRequest = 6,        // GET_DESCRIPTOR
            .wValue = (3 << 8) | 2,  // string descriptor, index 2
            .wIndex = 0x0409,     // English
            .wLength = sizeof(strBuf),
            .pData = strBuf,
            .wLenDone = 0
        };
        if ((*nd->dev)->DeviceRequest(nd->dev, &strReq) == kIOReturnSuccess &&
            strBuf[0] > 2 && strBuf[1] == 3) {
            // USB string descriptors are UTF-16LE; extract ASCII portion
            int sLen = (strBuf[0] - 2) / 2;
            if (sLen > 63) sLen = 63;
            for (int i = 0; i < sLen; i++)
                nd->productName[i] = strBuf[2 + i * 2];
            nd->productName[sLen] = '\0';
        } else {
            snprintf(nd->productName, sizeof(nd->productName),
                     "Nord (PID 0x%04X)", pid);
        }
    }

    // Read device info via vendor control
    uint8_t cb[4];
    IOUSBDevRequest req;

    req = (IOUSBDevRequest){0xC0, 0x04, 0, 0, 2, cb, 0};
    if ((*nd->dev)->DeviceRequest(nd->dev, &req) == kIOReturnSuccess)
        nd->fwVersion = cb[0] | (cb[1] << 8);

    req = (IOUSBDevRequest){0xC0, 0x05, 0, 0, 2, cb, 0};
    if ((*nd->dev)->DeviceRequest(nd->dev, &req) == kIOReturnSuccess)
        nd->fwBuild = cb[0] | (cb[1] << 8);

    req = (IOUSBDevRequest){0xC0, 0x08, 0, 0, 4, cb, 0};
    if ((*nd->dev)->DeviceRequest(nd->dev, &req) == kIOReturnSuccess)
        nd->maxBulkBuf = cb[0] | (cb[1] << 8) | (cb[2] << 16) | (cb[3] << 24);

    // Find vendor interface
    IOUSBFindInterfaceRequest ifReq = {255, 255, 255, kIOUSBFindInterfaceDontCare};
    io_iterator_t iter;
    (*nd->dev)->CreateInterfaceIterator(nd->dev, &ifReq, &iter);
    io_service_t ifSvc = IOIteratorNext(iter);
    IOObjectRelease(iter);

    if (!ifSvc) {
        // Only reset if needed
        (*nd->dev)->ResetDevice(nd->dev);
        usleep(200000);
        (*nd->dev)->SetConfiguration(nd->dev, 1);
        (*nd->dev)->CreateInterfaceIterator(nd->dev, &ifReq, &iter);
        ifSvc = IOIteratorNext(iter);
        IOObjectRelease(iter);
    }
    if (!ifSvc) {
        fprintf(stderr, "No vendor interface found\n");
        (*nd->dev)->USBDeviceClose(nd->dev);
        (*nd->dev)->Release(nd->dev);
        free(nd);
        return NULL;
    }

    IOCFPlugInInterface **ifPlug = NULL;
    IOCreatePlugInInterfaceForService(ifSvc, kIOUSBInterfaceUserClientTypeID,
                                       kIOCFPlugInInterfaceID, &ifPlug, &score);
    IOObjectRelease(ifSvc);
    (*ifPlug)->QueryInterface(ifPlug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID220),
                              (LPVOID*)&nd->intf);
    (*ifPlug)->Release(ifPlug);

    kr = (*nd->intf)->USBInterfaceOpenSeize(nd->intf);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "Failed to open interface: 0x%08x\n", kr);
        (*nd->dev)->USBDeviceClose(nd->dev);
        (*nd->dev)->Release(nd->dev);
        free(nd);
        return NULL;
    }

    // Assign pipes
    UInt8 nEP;
    (*nd->intf)->GetNumEndpoints(nd->intf, &nEP);
    for (UInt8 i = 1; i <= nEP; i++) {
        UInt8 dir, num, xfer, intv;
        UInt16 maxPkt;
        (*nd->intf)->GetPipeProperties(nd->intf, i, &dir, &num, &xfer, &maxPkt, &intv);
        if (xfer == kUSBInterrupt && dir == kUSBIn) nd->intPipe = i;
        else if (xfer == kUSBBulk && dir == kUSBOut) nd->bulkOutPipe = i;
        else if (xfer == kUSBBulk && dir == kUSBIn) nd->bulkInPipe = i;
    }

    if (!nd->intPipe || !nd->bulkInPipe || !nd->bulkOutPipe) {
        fprintf(stderr, "Missing required pipes\n");
        (*nd->intf)->USBInterfaceClose(nd->intf);
        (*nd->dev)->USBDeviceClose(nd->dev);
        (*nd->dev)->Release(nd->dev);
        free(nd);
        return NULL;
    }

    // No pipe recovery — clean open is sufficient after USB reset

    // Start I/O thread
    pthread_create(&nd->ioThread, NULL, ioThreadFunc, nd);
    while (!nd->runLoopReady) usleep(10000);
    usleep(50000);

    nd->connected = true;
    return nd;
}

void nord_close(NordDevice *nd) {
    if (!nd) return;

    if (nd->runLoop) CFRunLoopStop(nd->runLoop);
    usleep(200000);

    if (nd->intf) {
        (*nd->intf)->USBInterfaceClose(nd->intf);
        (*nd->intf)->Release(nd->intf);
        nd->intf = NULL;
    }
    if (nd->dev) {
        (*nd->dev)->USBDeviceClose(nd->dev);
        (*nd->dev)->Release(nd->dev);
        nd->dev = NULL;
    }

    pthread_mutex_destroy(&nd->respMutex);
    pthread_cond_destroy(&nd->respCond);
    nd->connected = false;
    free(nd);
}

// ─── Protocol operations ───

int nord_send_recv(NordDevice *nd,
                   uint32_t protoId, uint32_t subId, uint32_t msgType,
                   const uint8_t *payload, int payloadLen,
                   uint8_t *respBuf, int respBufSize,
                   int timeoutMs) {
    uint8_t msgBuf[NORD_MSG_BUF];
    int msgLen = nord_build_message(msgBuf, sizeof(msgBuf),
                                    protoId, subId, msgType,
                                    payload, payloadLen);
    if (msgLen < 0) return -1;

    // Clear response state
    pthread_mutex_lock(&nd->respMutex);
    nd->gotResponse = false;
    nd->respLen = 0;
    pthread_mutex_unlock(&nd->respMutex);

    // Send (with stall recovery)
    IOReturn kr = (*nd->intf)->WritePipeTO(nd->intf, nd->bulkOutPipe,
                                            msgBuf, msgLen, 1000, 5000);
    if (kr == (IOReturn)0xe0004051) { // kIOUSBPipeStalled
        (*nd->intf)->ClearPipeStallBothEnds(nd->intf, nd->bulkOutPipe);
        (*nd->intf)->ClearPipeStallBothEnds(nd->intf, nd->bulkInPipe);
        usleep(100000);
        kr = (*nd->intf)->WritePipeTO(nd->intf, nd->bulkOutPipe,
                                       msgBuf, msgLen, 1000, 5000);
    }
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "WritePipe failed: 0x%08x\n", kr);
        return -1;
    }

    // Wait for response
    pthread_mutex_lock(&nd->respMutex);
    if (!nd->gotResponse) {
        struct timespec ts;
        struct timeval tv;
        gettimeofday(&tv, NULL);
        ts.tv_sec = tv.tv_sec + timeoutMs / 1000;
        ts.tv_nsec = (tv.tv_usec + (timeoutMs % 1000) * 1000) * 1000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&nd->respCond, &nd->respMutex, &ts);
    }

    int result = -1;
    if (nd->gotResponse && nd->respLen > 0) {
        int copyLen = nd->respLen < (uint32_t)respBufSize ? nd->respLen : respBufSize;
        memcpy(respBuf, nd->respBuf, copyLen);
        result = nd->respLen;
    }
    pthread_mutex_unlock(&nd->respMutex);

    return result;
}

bool nord_handshake(NordDevice *nd) {
    uint8_t resp[256];
    // CQryProtocol uses sub=0 — version negotiation happens via the response
    int len = nord_send_recv(nd, PROTO_CTRL, 0, MSG_QRY_PROTOCOL,
                              NULL, 0, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    if (pid != PROTO_CTRL || mt != MSG_RPY_PROTOCOL) return false;

    // Parse protocol list
    if (len > NORD_HEADER_SIZE) {
        uint8_t count = resp[NORD_HEADER_SIZE];
        nd->numProtocols = count;
        int offset = NORD_HEADER_SIZE + 1;
        for (int i = 0; i < count && offset + 1 < len; i++) {
            uint8_t pId = resp[offset++];
            uint8_t ver = resp[offset++];
            if (pId < 16) {
                nd->protocols[pId].id = pId;
                nd->protocols[pId].version = ver;
                nd->protocols[pId].supported = true;
            }
        }
    }

    nd->handshakeComplete = true;
    return true;
}

bool nord_query_version(NordDevice *nd) {
    uint8_t resp[256];
    int len = nord_send_recv(nd, PROTO_CTRL, 0, MSG_QRY_VERSION,
                              NULL, 0, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    if (pid != PROTO_CTRL || mt != MSG_RPY_VERSION) return false;

    // The version response payload follows the header
    printf("  Version response (%d bytes):\n", len);
    nord_hexdump("payload", resp + NORD_HEADER_SIZE, len - NORD_HEADER_SIZE - NORD_CRC_SIZE);

    return true;
}

// ─── Helper: get negotiated version for a protocol ───

static uint32_t get_proto_ver(NordDevice *nd, uint8_t protoId) {
    if (protoId < 16 && nd->protocols[protoId].supported)
        return nd->protocols[protoId].version;
    // Defaults from binary analysis
    switch (protoId) {
        case PROTO_UI: return PROTO_VER_UI;
        case PROTO_CTRL: return PROTO_VER_CTRL;
        case PROTO_INSTRCTRL: return PROTO_VER_INSTRCTRL;
        case PROTO_FILETRANSFER: return PROTO_VER_FILETRANSFER;
        case PROTO_MIDIX: return PROTO_VER_MIDIX;
        default: return 0;
    }
}

// ─── UI operations ───

bool nord_ui_begin(NordDevice *nd) {
    uint8_t resp[256];
    uint32_t ver = get_proto_ver(nd, PROTO_UI);
    int len = nord_send_recv(nd, PROTO_UI, ver, MSG_UI_REQ_BEGIN,
                              NULL, 0, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    return pid == PROTO_UI && mt == MSG_UI_ACK_BEGIN;
}

bool nord_ui_end(NordDevice *nd) {
    uint8_t resp[256];
    uint32_t ver = get_proto_ver(nd, PROTO_UI);
    int len = nord_send_recv(nd, PROTO_UI, ver, MSG_UI_REQ_END,
                              NULL, 0, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    return pid == PROTO_UI && mt == MSG_UI_ACK_END;
}

// ─── Expose BE helpers ───

void nord_put_be32(uint8_t *p, uint32_t v) { put_be32(p, v); }
uint32_t nord_get_be32(const uint8_t *p) { return get_be32(p); }

// ─── FileTransfer operations ───

bool nord_ft_begin(NordDevice *nd) {
    uint8_t payload[4] = {0, 0, 0, 0}; // session context/ID = 0
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    uint8_t resp[256];
    int len = nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_REQ_BEGIN,
                              payload, 4, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    return pid == PROTO_FILETRANSFER && mt == MSG_FT_ACK_BEGIN;
}

bool nord_ft_begin_partition(NordDevice *nd, uint32_t partId) {
    uint8_t payload[4];
    put_be32(payload, partId);
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    uint8_t resp[256];
    int len = nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_REQ_BEGIN,
                              payload, 4, resp, sizeof(resp), 5000);
    if (len <= 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    return mt == MSG_FT_ACK_BEGIN && get_be32(resp + NORD_HEADER_SIZE) == 0;
}

bool nord_ft_end(NordDevice *nd) {
    uint8_t resp[256];
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    int len = nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_REQ_END,
                              NULL, 0, resp, sizeof(resp), 5000);
    if (len < 0) return false;

    uint32_t sz, pid, sid, mt;
    if (!nord_parse_header(resp, len, &sz, &pid, &sid, &mt)) return false;
    return pid == PROTO_FILETRANSFER;
}

int nord_ft_query_partitions(NordDevice *nd, uint8_t *respBuf, int respBufSize) {
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    return nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_QRY_PART_LIST,
                           NULL, 0, respBuf, respBufSize, 5000);
}

int nord_ft_query_part_state(NordDevice *nd, uint32_t partId,
                              uint8_t *respBuf, int respBufSize) {
    uint8_t payload[4];
    put_be32(payload, partId);
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    return nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_QRY_PART_STATE,
                           payload, 4, respBuf, respBufSize, 5000);
}

int nord_ft_query_file_iterate(NordDevice *nd, uint32_t partId,
                                uint8_t *respBuf, int respBufSize) {
    uint8_t payload[4];
    put_be32(payload, partId);
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    return nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_QRY_FILE_ITERATE,
                           payload, 4, respBuf, respBufSize, 10000);
}

int nord_ft_query_bank_list(NordDevice *nd, uint32_t partId,
                             uint8_t *respBuf, int respBufSize) {
    uint8_t payload[4];
    put_be32(payload, partId);
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    return nord_send_recv(nd, PROTO_FILETRANSFER, ver, MSG_FT_QRY_BANK_LIST,
                           payload, 4, respBuf, respBufSize, 5000);
}

bool nord_query_layout(NordDevice *nd, uint32_t partId) {
    // Query CRpyBankList to discover bank count and slots per bank.
    // Response format: [status U32] then N entries of
    //   [U32 nameLen][nameBytes...][U32 zero][U16 slotsPerBank]
    if (!nord_ft_begin_partition(nd, partId)) return false;

    uint8_t resp[4096];
    int len = nord_ft_query_bank_list(nd, partId, resp, sizeof(resp));
    nord_ft_end(nd);

    if (len <= 0) return false;

    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;

    // Response format: [U32 status][U32 partId][U8 bankCount]
    // then per bank: [U32 nameLen][nameBytes...][U32 slotsPerBank]
    if (pLen < 9) return false;

    uint32_t status = get_be32(d);
    if (status != 0) return false;

    int bankCount = d[8];  // U8 at offset 8
    int slotsPerBank = 0;

    // Walk entries to find slotsPerBank
    int pos = 9;
    for (int i = 0; i < bankCount && pos + 4 <= pLen; i++) {
        uint32_t nameLen = get_be32(d + pos);
        pos += 4;
        if (nameLen > 256 || pos + (int)nameLen + 4 > pLen) break;
        pos += (int)nameLen;  // skip name bytes
        if (pos + 4 > pLen) break;
        uint32_t slots = get_be32(d + pos);
        pos += 4;
        if (slotsPerBank == 0) slotsPerBank = (int)slots;
    }

    if (bankCount > 0) {
        nd->numBanks = bankCount;
        nd->slotsPerBank = slotsPerBank;
        return true;
    }
    return false;
}

// ─── High-level FileTransfer operations ───

int nord_ft_send(NordDevice *nd, uint32_t msgType,
                 const uint8_t *payload, int payloadLen,
                 uint8_t *resp, int respLen, int timeoutMs) {
    uint32_t ver = get_proto_ver(nd, PROTO_FILETRANSFER);
    return nord_send_recv(nd, PROTO_FILETRANSFER, ver, msgType,
                           payload, payloadLen, resp, respLen, timeoutMs);
}

int nord_ft_iterate(NordDevice *nd, uint32_t startIdx,
                    uint8_t *resp, int respLen) {
    uint8_t pay[12];
    put_be32(pay, startIdx);
    put_be32(pay + 4, 0xFFFFFFFF);  // category = all
    put_be32(pay + 8, 0);           // flags = 0
    return nord_ft_send(nd, MSG_FT_QRY_FILE_ITERATE, pay, 12, resp, respLen, 5000);
}

int nord_ft_file_info(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                      uint8_t *resp, int respLen) {
    uint8_t pay[8];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    return nord_ft_send(nd, MSG_FT_QRY_FILE_INFO, pay, 8, resp, respLen, 5000);
}

int nord_ft_get_dep(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                    uint8_t *resp, int respLen) {
    uint8_t pay[8];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    return nord_ft_send(nd, MSG_FT_QRY_FILE_GET_DEP, pay, 8, resp, respLen, 5000);
}

int nord_ft_get_focus(NordDevice *nd, uint32_t partId) {
    uint8_t pay[4], resp[256];
    put_be32(pay, partId);
    int len = nord_ft_send(nd, MSG_FT_QRY_FILE_GET_FOCUS, pay, 4, resp, sizeof(resp), 5000);
    if (len <= 0) return -1;
    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    if (pLen >= 4) return (int)get_be32(resp + NORD_HEADER_SIZE);
    return -1;
}

// Helper: send 16-byte management command (copy/move/swap)
static int ft_mgmt_16(NordDevice *nd, uint32_t msgType,
                       uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    uint8_t pay[16], resp[4096];
    put_be32(pay, a);
    put_be32(pay + 4, b);
    put_be32(pay + 8, c);
    put_be32(pay + 12, d);
    int len = nord_ft_send(nd, msgType, pay, 16, resp, sizeof(resp), 60000);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}

int nord_ft_copy(NordDevice *nd, uint32_t srcBank, uint32_t srcSlot,
                 uint32_t dstBank, uint32_t dstSlot) {
    return ft_mgmt_16(nd, MSG_FT_REQ_FILE_COPY, srcBank, srcSlot, dstBank, dstSlot);
}

int nord_ft_move(NordDevice *nd, uint32_t srcBank, uint32_t srcSlot,
                 uint32_t dstBank, uint32_t dstSlot) {
    return ft_mgmt_16(nd, MSG_FT_REQ_FILE_MOVE, srcBank, srcSlot, dstBank, dstSlot);
}

int nord_ft_swap(NordDevice *nd, uint32_t bank1, uint32_t slot1,
                 uint32_t bank2, uint32_t slot2) {
    return ft_mgmt_16(nd, MSG_FT_REQ_FILE_SWAP, bank1, slot1, bank2, slot2);
}

int nord_ft_rename(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                   const char *name) {
    int nameLen = (int)strlen(name);
    if (nameLen > 200) return -1;
    uint8_t pay[256], resp[4096];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    put_be32(pay + 8, (uint32_t)nameLen);
    memcpy(pay + 12, name, nameLen);
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_RENAME, pay, 12 + nameLen, resp, sizeof(resp), 60000);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}

int nord_ft_delete(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx) {
    uint8_t pay[8], resp[4096];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_DELETE, pay, 8, resp, sizeof(resp), 60000);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}

// ─── File read/write operations ───

int nord_ft_file_open(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                       uint32_t *fileSize) {
    uint8_t pay[8], resp[4096];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_OPEN, pay, 8, resp, sizeof(resp), 5000);
    if (len <= 0) return -1;
    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;
    int status = (int)get_be32(d);
    if (status == 0 && fileSize && pLen >= 8)
        *fileSize = get_be32(d + 4);
    return status;
}

int nord_ft_file_read(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                       uint32_t offset, uint32_t length,
                       uint8_t *dataBuf, uint32_t dataBufSize,
                       uint32_t *bytesRead) {
    uint8_t pay[16];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    put_be32(pay + 8, offset);
    put_be32(pay + 12, length);
    uint8_t resp[NORD_BULK_BUF];
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_READ, pay, 16, resp, sizeof(resp), 10000);
    if (len <= 0) return -1;
    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;
    int status = (int)get_be32(d);
    if (status != 0) return status;
    // Response: [U32 status][U32 bank][U32 slot][U32 offset][U32 dataLen][data...]
    if (pLen < 20) return -1;
    uint32_t dLen = get_be32(d + 16);
    if (dLen > dataBufSize) dLen = dataBufSize;
    if (20 + (int)dLen > pLen) dLen = pLen - 20;
    memcpy(dataBuf, d + 20, dLen);
    if (bytesRead) *bytesRead = dLen;
    return 0;
}

int nord_ft_file_close(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx) {
    uint8_t pay[8], resp[4096];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_CLOSE, pay, 8, resp, sizeof(resp), 5000);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}

int nord_ft_file_create(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                         uint32_t fileSize, uint32_t fileType,
                         uint32_t category, const char *name) {
    // CReqFileCreate payload (from NSM binary disassembly):
    //   U32 bank
    //   U32 entry (slot)
    //   U32 size
    //   U32 fileType
    //   U32 category
    //   U32 timestamp (unix epoch)
    //   U32 nameLength
    //   U8[N] name bytes

    int nameLen = name ? (int)strlen(name) : 0;
    if (nameLen > 128) nameLen = 128;

    int payLen = 28 + nameLen;
    uint8_t pay[256], resp[4096];
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    put_be32(pay + 8, fileSize);
    put_be32(pay + 12, fileType);
    put_be32(pay + 16, category);

    // Timestamp: current time as Unix epoch
    struct timeval tv;
    gettimeofday(&tv, NULL);
    put_be32(pay + 20, (uint32_t)tv.tv_sec);

    put_be32(pay + 24, (uint32_t)nameLen);
    if (nameLen > 0)
        memcpy(pay + 28, name, nameLen);

    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_CREATE, pay, payLen, resp, sizeof(resp), 10000);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}

int nord_ft_file_write(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                        uint32_t offset, const uint8_t *data, uint32_t dataLen) {
    // Payload: [U32 bank][U32 slot][U32 offset][U32 dataLen][data...]
    uint32_t payLen = 16 + dataLen;
    uint8_t *pay = malloc(payLen);
    if (!pay) return -1;
    put_be32(pay, bankIdx);
    put_be32(pay + 4, slotIdx);
    put_be32(pay + 8, offset);
    put_be32(pay + 12, dataLen);
    memcpy(pay + 16, data, dataLen);
    uint8_t resp[4096];
    int len = nord_ft_send(nd, MSG_FT_REQ_FILE_WRITE, pay, payLen, resp, sizeof(resp), 10000);
    free(pay);
    if (len <= 0) return -1;
    return (int)get_be32(resp + NORD_HEADER_SIZE);
}
