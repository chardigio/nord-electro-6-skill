/*
 * nord_usb.h — Nord Electro 6 USB protocol library
 *
 * Provides connection management, message framing, and protocol operations
 * for communicating with Nord keyboards over their vendor USB interface.
 */

#ifndef NORD_USB_H
#define NORD_USB_H

#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>

// ─── Constants ───

#define NORD_VID 0x0FFC            // Clavia DMI AB
#define NORD_PID_ELECTRO6 0x0029   // Nord Electro 6 (for reference; detection is VID-only)

// Protocol IDs (confirmed from binary + device response)
#define PROTO_DUMMY       0
#define PROTO_UI          6
#define PROTO_CTRL        7
#define PROTO_INSTRCTRL  10
#define PROTO_FILETRANSFER 12
#define PROTO_MIDIX      13

// Ctrl message types
#define MSG_QRY_VERSION      0
#define MSG_RPY_VERSION      1
#define MSG_QRY_PROTOCOL     2
#define MSG_RPY_PROTOCOL     3
#define MSG_REQ_REBOOT       4
#define MSG_REQ_BOOT         5
#define MSG_QRY_VERSION_SPEC 6
#define MSG_RPY_VERSION_SPEC 7
#define MSG_REQ_SET_SERIAL   8
#define MSG_ACK_SET_SERIAL   9

// FileTransfer message types (CONFIRMED from binary analysis)
#define MSG_FT_QRY_PART_LIST      0
#define MSG_FT_RPY_PART_LIST      1
#define MSG_FT_QRY_BANK_LIST      2
#define MSG_FT_RPY_BANK_LIST      3
#define MSG_FT_REQ_BEGIN          4   // payload: 1x U32
#define MSG_FT_ACK_BEGIN          5
#define MSG_FT_REQ_END            6
#define MSG_FT_QRY_PART_STATE     8
#define MSG_FT_RPY_PART_STATE     9
#define MSG_FT_REQ_FILE_CREATE   10
#define MSG_FT_ACK_FILE_CREATE   11
#define MSG_FT_REQ_FILE_OPEN     12
#define MSG_FT_ACK_FILE_OPEN     13
#define MSG_FT_REQ_FILE_CLOSE    14
#define MSG_FT_ACK_FILE_CLOSE    15
#define MSG_FT_REQ_FILE_WRITE    16
#define MSG_FT_ACK_FILE_WRITE    17
#define MSG_FT_REQ_FILE_READ     18
#define MSG_FT_ACK_FILE_READ     19
#define MSG_FT_REQ_FILE_DELETE   20
#define MSG_FT_ACK_FILE_DELETE   21
#define MSG_FT_REQ_FILE_COPY     22
#define MSG_FT_ACK_FILE_COPY     23
#define MSG_FT_REQ_FILE_MOVE     24
#define MSG_FT_ACK_FILE_MOVE     25
#define MSG_FT_REQ_FILE_SWAP     26
#define MSG_FT_ACK_FILE_SWAP     27
#define MSG_FT_REQ_FILE_RENAME   28
#define MSG_FT_ACK_FILE_RENAME   29
#define MSG_FT_QRY_FILE_INFO     30
#define MSG_FT_RPY_FILE_INFO     31
#define MSG_FT_QRY_FILE_ITERATE  32
#define MSG_FT_RPY_FILE_ITERATE  33
#define MSG_FT_REQ_ERASE_BLOCK   34
#define MSG_FT_REQ_ERASE_ALL     36
#define MSG_FT_QRY_ERASE_STATUS  38
#define MSG_FT_RPY_ERASE_STATUS  39
#define MSG_FT_QRY_FILE_GET_DEP  40
#define MSG_FT_RPY_FILE_GET_DEP  41
#define MSG_FT_INVALIDATE_PART   42
#define MSG_FT_INVALIDATE_BANK   43
#define MSG_FT_INVALIDATE_FILE   44
#define MSG_FT_REQ_INVALIDATE_EN 45  // payload: 2x U32
#define MSG_FT_ACK_INVALIDATE_EN 46
#define MSG_FT_REQ_FILE_SET_FOCUS 47
#define MSG_FT_ACK_FILE_SET_FOCUS 48
#define MSG_FT_QRY_FILE_GET_FOCUS 49
#define MSG_FT_RPY_FILE_GET_FOCUS 50
#define MSG_FT_REQ_FILE_SET_CAT  51
#define MSG_FT_ACK_FILE_SET_CAT  52
#define MSG_FT_REQ_FILE_SET_DEP  53
#define MSG_FT_ACK_FILE_SET_DEP  54
#define MSG_FT_REQ_ERASE_CANCEL  55
#define MSG_FT_REQ_RESET         57
#define MSG_FT_REQ_FILE_CONVERT  59
#define MSG_FT_ACK_FILE_CONVERT  60
#define MSG_FT_QRY_CONTENT_VER   61
#define MSG_FT_RPY_CONTENT_VER   62

// UI message types (CONFIRMED from binary analysis)
#define MSG_UI_REQ_BEGIN     0
#define MSG_UI_ACK_BEGIN     1
#define MSG_UI_REQ_END       2
#define MSG_UI_ACK_END       3
#define MSG_UI_QRY_INFO      4
#define MSG_UI_RPY_INFO      5
#define MSG_UI_REQ_TEXT      6   // payload: string
#define MSG_UI_REQ_PROGRESS  7   // payload: 4x U8

// InstrCtrl message types
#define MSG_IC_REQ_SET_INSTR       0
#define MSG_IC_ACK_SET_INSTR       1
#define MSG_IC_QRY_GET_INSTR       2
#define MSG_IC_RPY_GET_INSTR       3
#define MSG_IC_REQ_STROKE_ENABLE  16  // payload: 1x U8
#define MSG_IC_ACK_STROKE_ENABLE  17

// MIDIX message types
#define MSG_MX_REQ_UPSTREAM_EN    0   // payload: 1x U8
#define MSG_MX_ACK_UPSTREAM_EN    1

// Protocol versions (from CRpyProtocol and binary analysis)
#define PROTO_VER_UI          1
#define PROTO_VER_CTRL        2
#define PROTO_VER_INSTRCTRL   2  // from device handshake
#define PROTO_VER_FILETRANSFER 10 // from device handshake
#define PROTO_VER_MIDIX       0

// Buffer sizes
#define NORD_INTERRUPT_BUF  16
#define NORD_BULK_BUF       49152  // 0xC000
#define NORD_MSG_BUF        4096
#define NORD_HEADER_SIZE    16
#define NORD_CRC_SIZE       2

// ─── Types ───

typedef struct {
    uint8_t id;
    uint8_t version;
    bool supported;
} NordProtocolInfo;

typedef struct {
    // Device handles
    IOUSBDeviceInterface182 **dev;
    IOUSBInterfaceInterface220 **intf;

    // Pipe refs
    UInt8 intPipe;
    UInt8 bulkInPipe;
    UInt8 bulkOutPipe;

    // I/O thread
    pthread_t ioThread;
    CFRunLoopRef runLoop;
    volatile bool runLoopReady;

    // Read buffers
    uint8_t intBuf[NORD_INTERRUPT_BUF];
    uint8_t bulkBuf[NORD_BULK_BUF];

    // Response state
    pthread_mutex_t respMutex;
    pthread_cond_t respCond;
    volatile bool gotResponse;
    uint8_t respBuf[NORD_BULK_BUF];
    uint32_t respLen;

    // Device info
    uint16_t fwVersion;
    uint16_t fwBuild;
    uint32_t maxBulkBuf;
    uint16_t productId;
    char productName[64];  // from USB string descriptor

    // Protocol map (from CRpyProtocol)
    NordProtocolInfo protocols[16];
    int numProtocols;

    // Memory layout (from CRpyBankList, per partition)
    int numBanks;       // e.g. 26 for NE6 Program partition
    int slotsPerBank;   // e.g. 16 for NE6

    // Connection state
    bool connected;
    bool handshakeComplete;
} NordDevice;

// ─── Message building ───

// CRC-16/CCITT (confirmed: poly=0x1021, init=0xFFFF)
uint16_t nord_crc16(const uint8_t *data, int len);

// Build a protocol message. Returns total message length.
// payload can be NULL if payloadLen is 0.
int nord_build_message(uint8_t *buf, int bufSize,
                       uint32_t protoId, uint32_t subId, uint32_t msgType,
                       const uint8_t *payload, int payloadLen);

// Parse a response message header. Returns true if valid.
bool nord_parse_header(const uint8_t *data, int len,
                       uint32_t *totalSize, uint32_t *protoId,
                       uint32_t *subId, uint32_t *msgType);

// ─── Connection lifecycle ───

// Open connection to Nord device. Returns NULL on failure.
NordDevice *nord_open(void);

// Close connection and free resources.
void nord_close(NordDevice *nd);

// ─── Protocol operations ───

// Send a message and wait for response. Returns response length, or -1 on error.
// Response is copied into respBuf (caller provides).
int nord_send_recv(NordDevice *nd,
                   uint32_t protoId, uint32_t subId, uint32_t msgType,
                   const uint8_t *payload, int payloadLen,
                   uint8_t *respBuf, int respBufSize,
                   int timeoutMs);

// Perform protocol handshake (CQryProtocol). Populates nd->protocols.
bool nord_handshake(NordDevice *nd);

// Query firmware version. Populates nd->fwVersion.
bool nord_query_version(NordDevice *nd);

// ─── UI operations ───

// Begin/end UI session (shows progress on device screen).
bool nord_ui_begin(NordDevice *nd);
bool nord_ui_end(NordDevice *nd);

// ─── FileTransfer operations ───

// Begin file transfer session (legacy — sends partId=0).
bool nord_ft_begin(NordDevice *nd);

// Begin file transfer session for a specific partition.
// partId: 0=Piano Native, 1=Piano, 2=SampLib Native, 3=SampLib, 4=Program, 5=Live, 6=Settings
bool nord_ft_begin_partition(NordDevice *nd, uint32_t partId);

// End file transfer session.
bool nord_ft_end(NordDevice *nd);

// Query partition list. Returns partition data in respBuf.
int nord_ft_query_partitions(NordDevice *nd, uint8_t *respBuf, int respBufSize);

// Query partition state.
int nord_ft_query_part_state(NordDevice *nd, uint32_t partId,
                              uint8_t *respBuf, int respBufSize);

// Iterate files in a partition.
int nord_ft_query_file_iterate(NordDevice *nd, uint32_t partId,
                                uint8_t *respBuf, int respBufSize);

// Query bank list for a partition. Returns response length.
int nord_ft_query_bank_list(NordDevice *nd, uint32_t partId,
                             uint8_t *respBuf, int respBufSize);

// Query memory layout (bank count, slots per bank) for a partition.
// Populates nd->numBanks and nd->slotsPerBank. Returns true on success.
bool nord_query_layout(NordDevice *nd, uint32_t partId);

// ─── High-level FileTransfer operations ───
// These send raw FT messages; caller must have an active partition session.

// Send a FileTransfer message and return response length (-1 on error).
int nord_ft_send(NordDevice *nd, uint32_t msgType,
                 const uint8_t *payload, int payloadLen,
                 uint8_t *resp, int respLen, int timeoutMs);

// Iterate banks. Returns response length. Caller parses status/bankIdx/activeSlot.
// Payload: [startIdx U32][category U32 = 0xFFFFFFFF][flags U32 = 0]
int nord_ft_iterate(NordDevice *nd, uint32_t startIdx,
                    uint8_t *resp, int respLen);

// Get file info. Payload: [bankIdx U32][slotIdx U32] (partition set by CReqBegin)
int nord_ft_file_info(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                      uint8_t *resp, int respLen);

// Get file dependencies. Payload: [bankIdx U32][slotIdx U32]
int nord_ft_get_dep(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                    uint8_t *resp, int respLen);

// Get focused bank. Payload: [partId U32]. Returns bank index or -1.
int nord_ft_get_focus(NordDevice *nd, uint32_t partId);

// Copy bank:slot → bank:slot. Returns status (0=success).
int nord_ft_copy(NordDevice *nd, uint32_t srcBank, uint32_t srcSlot,
                 uint32_t dstBank, uint32_t dstSlot);

// Move bank:slot → bank:slot. Returns status (0=success).
int nord_ft_move(NordDevice *nd, uint32_t srcBank, uint32_t srcSlot,
                 uint32_t dstBank, uint32_t dstSlot);

// Swap two bank:slot locations. Returns status (0=success).
int nord_ft_swap(NordDevice *nd, uint32_t bank1, uint32_t slot1,
                 uint32_t bank2, uint32_t slot2);

// Rename a bank:slot. Returns status (0=success).
int nord_ft_rename(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                   const char *name);

// Delete a bank:slot. Returns status (0=success).
int nord_ft_delete(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx);

// ─── File read/write operations ───

// Open file for reading. Returns status (0=success). Sets *fileSize if non-NULL.
int nord_ft_file_open(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                       uint32_t *fileSize);

// Read file data. Returns status (0=success). Copies data to dataBuf.
int nord_ft_file_read(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                       uint32_t offset, uint32_t length,
                       uint8_t *dataBuf, uint32_t dataBufSize,
                       uint32_t *bytesRead);

// Close file handle. Returns status (0=success).
int nord_ft_file_close(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx);

// Create file for writing. Returns status (0=success).
// fileType: opaque U32 from CFileType::GetType() (extract from CBIN header)
// category: 0xFFFFFFFF for uncategorized
// name: program name (max 128 chars, NULL for no name)
int nord_ft_file_create(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                         uint32_t fileSize, uint32_t fileType,
                         uint32_t category, const char *name);

// Write data to file. Returns status (0=success).
int nord_ft_file_write(NordDevice *nd, uint32_t bankIdx, uint32_t slotIdx,
                        uint32_t offset, const uint8_t *data, uint32_t dataLen);

// ─── Utility ───

void nord_hexdump(const char *label, const uint8_t *data, int len);

// Big-endian helpers (exposed for callers parsing raw responses)
void nord_put_be32(uint8_t *p, uint32_t v);
uint32_t nord_get_be32(const uint8_t *p);

#endif // NORD_USB_H
