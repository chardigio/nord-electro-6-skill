# Nord Electro 6 USB Protocol — Reverse Engineering Documentation

**Date**: 2026-03-21
**Status**: Phase 2 COMPLETE — CLI with per-program names, file read, and all management operations
**Last Updated**: 2026-03-22 — Fixed payload addressing: [bank][slot] not [partId][bank]

---

## 1. Project Goal

Build a CLI tool that connects to a Nord Electro 6 keyboard over USB and manages program presets via an AI-powered setlist workflow. The agent reads a setlist, matches songs to existing presets, and arranges them contiguously on the keyboard — copying/renaming similar presets for songs that don't have a dedicated one.

---

## 2. Hardware Summary

| Property | Value |
|----------|-------|
| Instrument | Nord Electro 6 |
| Manufacturer | Clavia DMI AB |
| USB VID | `0x0FFC` |
| USB PID | `0x0029` (decimal 41 — matches Clavia instrument ID) |
| USB Speed | Full Speed (12 Mbps) |
| bcdDevice | `0x0266` (firmware v2.66) |
| USB Driver (macOS) | `com.apple.AppleMIDIUSBDriver` (MIDI interface only; vendor interface has no OS driver) |
| macOS IOKit API | Classic `IOUSBDeviceInterface182` / `IOUSBInterfaceInterface220` (NOT IOUSBHost) |

### NE6 Memory Layout

- **416 program slots**: 26 banks (A–Z) × 16 programs per bank
- Each bank: 4 pages × 4 programs
- MIDI addressing: CC0 (Bank Select MSB) = 0, CC32 (Bank Select LSB) = 0–25 (A=0, Z=25), Program Change = 0–15
- **No setlist mode** — removed from Electro 5. This tool fills that gap.

### NE6 File Types

| Extension | Description |
|-----------|-------------|
| `.ne6p` | Program |
| `.ne6b` | Backup (zip archive internally) |
| `.ne6l` | List |
| `.ne6t` | Song |
| `.ne6pbundle` | Program bundle |

All files use a `CBIN` magic header followed by the format identifier (e.g. `ne6p`).

---

## 3. USB Interface Layout

The Nord Electro 6 exposes **3 USB interfaces**:

| Interface | Class | SubClass | Protocol | Endpoints | Purpose |
|-----------|-------|----------|----------|-----------|---------|
| **0** | **255 (Vendor Specific)** | 255 | 255 | **3** | **Zevs protocol — Nord Sound Manager communication** |
| 1 | 1 (Audio) | 1 (Control) | 0 | 0 | Audio control (MIDI) |
| 2 | 1 (Audio) | 3 (MIDI Streaming) | 0 | 2 | Standard USB MIDI (what CoreMIDI sees) |

### Interface 0 Endpoints (Vendor Specific — THE protocol interface)

| Endpoint | Address | Direction | Type | Max Packet | Purpose |
|----------|---------|-----------|------|------------|---------|
| EP1 | `0x81` | IN (device→host) | **Interrupt** | 16 bytes | Status/notifications |
| EP2 | `0x82` | IN (device→host) | **Bulk** | 64 bytes | Protocol responses |
| EP3 | `0x03` | OUT (host→device) | **Bulk** | 64 bytes | Protocol commands |

### Interface 2 Endpoints (MIDI — separate, not used by protocol)

| Endpoint | Address | Direction | Type | Max Packet |
|----------|---------|-----------|------|------------|
| EP4 | `0x04` | OUT | Bulk | 64 bytes |
| EP4 | `0x84` | IN | Bulk | 64 bytes |

### USB String Descriptors

| Index | String |
|-------|--------|
| 1 | Clavia DMI AB |
| 2 | Nord Electro 6 |
| 3 | Nord Electro 6 |
| 7 | Nord Electro 6 MIDI |
| 8 | Nord Electro 6 MIDI Input |
| 9 | Nord Electro 6 MIDI Output |
| 10 | Nord Electro 6 MIDI Element |

### Confirmed: Same layout across Nord keyboard line

A `lsusb` dump of a Nord Piano 4 (PID `0x002A`) from the Linux hardware database shows an identical interface/endpoint layout. This is a common hardware design across the modern Nord line.

---

## 4. Key Finding: Nord Sound Manager Uses Raw USB, NOT MIDI SysEx

Nord Sound Manager (`/Applications/Nord Sound Manager v9.16.app`) imports **zero CoreMIDI symbols**. It uses `IOKit.framework` with `IOCreatePlugInInterfaceForService` for direct USB access to the vendor-specific Interface 0.

MIDI Monitor (with spy-on-destinations enabled) shows **zero SysEx traffic** during Nord Sound Manager operations — only standard CC/Program Change messages from the keyboard itself appear on the MIDI interface.

---

## 5. Vendor Control Transfers (Confirmed Working)

Before opening the bulk/interrupt pipes, the app queries device capabilities via USB vendor control requests on **endpoint 0** (the default control endpoint).

### Device-Level Requests (`bmRequestType = 0xC0` for IN, `0x40` for OUT)

| bRequest | Direction | wValue | wIndex | wLength | Purpose | Our Result |
|----------|-----------|--------|--------|---------|---------|------------|
| `0x00` | IN | 0 | 0 | 2 | **Get Model ID** | `00 00` |
| `0x01` | IN | 0 | 0 | 2 | (Unknown — possibly older API) | `0C 04` |
| `0x04` | IN | 0 | 0 | 2 | **Get Software Version** | `0A 01` |
| `0x05` | IN | 0 | 0 | 2 | **Get Software Build** | `D0 02` (build 720) |
| `0x08` | IN | 0 | 0 | 4 | **Get MaxBulkBufferSize** | `00 80 00 00` (32768) |

### Interface-Level Requests (`bmRequestType = 0xC1` for IN, `0x41` for OUT)

| bRequest | Direction | wValue | wIndex | wLength | Purpose | Our Result |
|----------|-----------|--------|--------|---------|---------|------------|
| `0x06` | IN | 0 | 0 | varies | **Get Interface Name** | `"Interface 0 user text"` |
| `0x09` | IN | 0 | ifNum | 2 | **Get Interface Type** | `00 00` (type 0 = USB/Control) |

### Vendor Request Enum (`Zevs::USB::Request::Vendor::ECode`)

From binary analysis, the request codes used by Nord Sound Manager during init:

```
0x00 = GetModel
0x04 = GetVersion
0x05 = GetBuild
0x08 = GetMaxBulkBufferSize
0x09 = GetInterfaceType (interface-level)
```

### Init_ReqDevice Helper (from disassembly)

```c
void Init_ReqDevice(IOUSBDevRequest *req, int direction, uint8_t code, uint16_t length, uint16_t value) {
    req->bmRequestType = 0x40 | (direction << 7);  // 0x40=OUT, 0xC0=IN
    req->bRequest = code;
    req->wValue = value;
    req->wIndex = 0;
    req->wLength = length;
}
```

---

## 6. Internal Architecture: Zevs + Ymer

Nord Sound Manager is built on two C++ libraries:

- **Zevs** — Low-level protocol/transport layer (message serialization, port abstraction, bitstream I/O, CRC)
- **Ymer** — High-level application framework (USB device management, protocol orchestration, file manager, product definitions)

### Naming Conventions

| Prefix | Meaning |
|--------|---------|
| `C` | Class (concrete) |
| `I` | Interface (abstract) |
| `S` | Struct |
| `E` | Enum |
| `Qry` | Query — host→device, expects reply data |
| `Rpy` | Reply — device→host response to query |
| `Req` | Request — host→device command, expects acknowledgment |
| `Ack` | Acknowledgment — device→host response to request |

---

## 7. Protocol Message Framing

Every message on the bulk USB pipes follows this format:

```
Byte 0–3:   uint32  total_size       (big-endian; includes all bytes including the 2-byte CRC)
Byte 4–7:   uint32  protocol_id      (identifies which protocol handler)
Byte 8–11:  uint32  sub_protocol_id  (actually the PROTOCOL VERSION — see note below)
Byte 12–15: uint32  message_type     (command ID within the protocol)
Byte 16+:   payload                  (bit-packed via Zevs::CBitStreamI/O)
Last 2:     uint16  CRC-16           (big-endian)
```

### From Disassembly — MsgProlog (`0x100089818`)

```c
void MsgProlog(CProtocolBase *proto, CBitStreamO *stream, uint32_t msgType, SInfoO *info) {
    stream->SetPos(0x20);                          // Skip 32 bits (4 bytes) for size field
    stream->Wr_U32(proto->GetProtocolID());        // Protocol ID
    stream->Wr_U32(proto->subId);                  // Sub-protocol ID
    stream->Wr_U32(msgType);                       // Message type
    if (info) info->flags = 0;
}
```

### From Disassembly — MsgEpilog (`0x100089890`)

```c
void MsgEpilog(CProtocolBase *proto, CBitStreamO *stream) {
    uint32_t pos = stream->GetPos() / 8;           // Current position in bytes
    uint32_t totalSize = pos + 2;                   // +2 for CRC
    stream->WrAtPos_U(0, totalSize, 32);            // Write total size at bit pos 0, 32 bits
    void *buffer = stream->GetBuffer();
    CCRC16 crc(buffer, pos);                        // CRC over all bytes up to current pos
    stream->Wr_U16(crc.Get());                      // Append CRC-16
}
```

### From Disassembly — OnReceive (`0x1000896c4`)

```c
int OnReceive(CBitStreamI &stream) {
    uint32_t totalSize = stream.Rd_U32();                    // Read declared size
    if (totalSize != stream.GetSize() / 8) return 3;        // Size mismatch error
    uint16_t payloadSize = totalSize - 2;                     // Subtract CRC
    uint16_t receivedCRC = stream.RdAtPos_U(payloadSize * 8, 16);
    CCRC16 crc(stream.GetBuffer(), payloadSize);
    if (crc.Get() != receivedCRC) return 2;                   // CRC mismatch error
    uint32_t protocolId = stream.Rd_U32();                    // Protocol ID
    if (protocolId > 0x0F) return 4;                          // Invalid protocol error
    uint32_t subId = stream.Rd_U32();                         // Sub-protocol
    uint32_t msgType = stream.Rd_U32();                       // Message type
    // Dispatch to registered protocol handler...
}
```

### CRC-16 Details — CONFIRMED

**CRC-16/CCITT**: polynomial `0x1021`, init `0xFFFF`, no reflection, big-endian output.

Confirmed from disassembly of `CCRC16` constructor (`movw $0xffff, (%rdi)`) and validated by successful protocol handshake.

Note: The Nord G2 uses init=0x0000 (XMODEM), but the modern Zevs protocol uses init=0xFFFF (CCITT).

CRC is computed over all bytes from position 0 through `(totalSize - 2)`, inclusive of the size field. CRC bytes are written big-endian (MSB first).

### CRC-16/CCITT Reference Implementation

```c
// CONFIRMED WORKING with Nord Electro 6
uint16_t crc16_ccitt(const uint8_t *data, int len) {
    uint16_t crc = 0xFFFF;
    for (int i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc & 0xFFFF;
}
```

### sub_protocol_id Is Actually a VERSION Field

The `sub_protocol_id` field (bytes 8–11) carries the **negotiated protocol version**, not a sub-protocol identifier. Each protocol has a version discovered during the `CRpyProtocol` handshake. All subsequent messages for that protocol must use the negotiated version in this field.

**Exception**: `CQryProtocol` itself (the version negotiation message) uses `sub_protocol_id = 0` because versions haven't been negotiated yet.

| Protocol | Version (NE6 fw 2.66) | sub_protocol_id value |
|----------|----------------------|----------------------|
| Ctrl (7) | 0 | 0 (for CQryProtocol), 0 (for other Ctrl msgs) |
| UI (6) | 1 | 1 |
| InstrCtrl (10) | 2 | 2 |
| FileTransfer (12) | 10 | 10 |
| MIDIX (13) | 0 | 0 |

---

## 8. Protocol Subsystems

Five protocol subsystems share a single multiplexed USB bulk pipe, distinguished by the `protocol_id` field:

| Protocol ID | Namespace | Purpose | Version (NE6 fw 2.66) |
|-------------|-----------|---------|----------------------|
| **6** | `Zevs::Protocol::UI` | Progress display, text on device screen | v1 |
| **7** | `Zevs::Protocol::Ctrl` | Device identification, version queries, protocol negotiation | v0 |
| **10** (0xA) | `Zevs::Protocol::InstrCtrl` | Real-time instrument parameter control | v2 |
| **12** (0xC) | `Zevs::Protocol::FileTransfer` | File operations (the main one we need) | v10 |
| **13** (0xD) | `Zevs::Protocol::MIDIX` | Extended MIDI note events | v0 |
| 0 | `Zevs::Protocol::Dummy` | Placeholder (no-op) | — |

**CONFIRMED** from binary analysis (`GetProtocolID()` return values) and live `CRpyProtocol` response (2026-03-22).

### 8.1 Ctrl Protocol — Message Types

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 0 | `CQryVersion` | Host→Device | Query firmware version |
| 1 | `CRpyVersion` | Device→Host | Firmware version response |
| 2 | `CQryProtocol` | Host→Device | Query supported protocols + versions |
| 3 | `CRpyProtocol` | Device→Host | Protocol support map |
| 4 | `CReqReboot` | Host→Device | Reboot instrument |
| 5 | `CReqBoot` | Host→Device | Boot instrument |
| 6 | `CQryVersionSpec` | Host→Device | Query specific version info |
| 7 | `CRpyVersionSpec` | Device→Host | Specific version response |
| 8 | `CReqSetSerial` | Host→Device | Set serial number |
| 9 | `CAckSetSerial` | Device→Host | Serial set acknowledgment |

### CRpyProtocol Response Format (from disassembly at `0x10008a800`)

```c
void CRpyProtocol::Read(CBitStreamI &stream) {
    MsgProlog(stream, 3, &info);          // Parse header, expect msg_type=3
    uint8_t numProtocols = stream.Rd_U8(); // Number of supported protocols
    for (int i = 0; i < numProtocols; i++) {
        uint8_t protocolId = stream.Rd_U8();
        uint8_t version = stream.Rd_U8();
        if (protocolId <= 0x0F) {
            supported[protocolId] = true;
            versions[protocolId] = version;
        }
    }
    MsgEpilog(stream);                     // Consume CRC
}
```

### 8.2 FileTransfer Protocol — Message Types (CONFIRMED IDs from Binary Analysis)

**Session Control:**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 4 | `CReqBegin` | Host→Device | Start file transfer session (payload: 1x U32) |
| 5 | `CAckBegin` | Device→Host | Session started |
| 6 | `CReqEnd` | Host→Device | End file transfer session |
| 57 | `CReqReset` | Host→Device | Reset/abort session |

**Partition/Bank Enumeration:**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 0 | `CQryPartList` | Host→Device | List partitions |
| 1 | `CRpyPartList` | Device→Host | Partition list response |
| 2 | `CQryBankList` | Host→Device | List banks in partition |
| 3 | `CRpyBankList` | Device→Host | Bank list response |
| 8 | `CQryPartState` | Host→Device | Partition state/status |
| 9 | `CRpyPartState` | Device→Host | Partition state response |

**File Operations (the core operations for setlist management):**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 10 | `CReqFileCreate` | Host→Device | Create new file |
| 11 | `CAckFileCreate` | Device→Host | File created |
| 12 | `CReqFileOpen` | Host→Device | Open existing file |
| 13 | `CAckFileOpen` | Device→Host | File opened |
| 14 | `CReqFileClose` | Host→Device | Close file handle |
| 15 | `CAckFileClose` | Device→Host | File closed |
| 16 | `CReqFileWrite` | Host→Device | Write data to file |
| 17 | `CAckFileWrite` | Device→Host | Write acknowledged |
| 18 | `CReqFileRead` | Host→Device | Read data from file |
| 19 | `CAckFileRead` | Device→Host | Read data response |
| 20 | `CReqFileDelete` | Host→Device | Delete file |
| 21 | `CAckFileDelete` | Device→Host | File deleted |
| 22 | `CReqFileCopy` | Host→Device | **Copy file within device** |
| 23 | `CAckFileCopy` | Device→Host | Copy acknowledged |
| 24 | `CReqFileMove` | Host→Device | **Move file to different slot** |
| 25 | `CAckFileMove` | Device→Host | Move acknowledged |
| 26 | `CReqFileSwap` | Host→Device | **Swap two file locations** |
| 27 | `CAckFileSwap` | Device→Host | Swap acknowledged |
| 28 | `CReqFileRename` | Host→Device | **Rename file** |
| 29 | `CAckFileRename` | Device→Host | Rename acknowledged |
| 59 | `CReqFileConvert` | Host→Device | Convert file format in-place |
| 60 | `CAckFileConvert` | Device→Host | Convert acknowledged |

**File Metadata:**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 30 | `CQryFileInfo` | Host→Device | Get file info |
| 31 | `CRpyFileInfo` | Device→Host | File info response |
| 32 | `CQryFileIterate` | Host→Device | Iterate files in partition |
| 33 | `CRpyFileIterate` | Device→Host | File iteration response |
| 49 | `CQryFileGetFocus` | Host→Device | Get currently focused file |
| 50 | `CRpyFileGetFocus` | Device→Host | Focus response |
| 47 | `CReqFileSetFocus` | Host→Device | Set focus to a slot |
| 48 | `CAckFileSetFocus` | Device→Host | Focus set acknowledged |
| 51 | `CReqFileSetCategory` | Host→Device | Set file category |
| 52 | `CAckFileSetCategory` | Device→Host | Category set acknowledged |
| 40 | `CQryFileGetDependency` | Host→Device | Get file dependencies |
| 41 | `CRpyFileGetDependency` | Device→Host | Dependency response |
| 53 | `CReqFileSetDependency` | Host→Device | Set file dependencies |
| 54 | `CAckFileSetDependency` | Device→Host | Dependency set acknowledged |

**Storage Management:**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 34 | `CReqEraseBlock` | Host→Device | Erase flash block |
| 36 | `CReqEraseAll` | Host→Device | Erase all in partition |
| 38 | `CQryEraseStatus` | Host→Device | Poll erase progress |
| 39 | `CRpyEraseStatus` | Device→Host | Erase status response |
| 55 | `CReqEraseCancel` | Host→Device | Cancel erase |

**Change Notifications:**

| ID | Message | Direction | Description |
|----|---------|-----------|-------------|
| 42 | `CInvalidatePart` | Device→Host | Partition changed |
| 43 | `CInvalidateBank` | Device→Host | Bank changed |
| 44 | `CInvalidateFile` | Device→Host | File changed |
| 45 | `CReqInvalidateEnable` | Host→Device | Enable/disable notifications (payload: 2x U32) |
| 46 | `CAckInvalidateEnable` | Device→Host | Enable acknowledged |
| 61 | `CQryContentVersion` | Host→Device | Query content version counter |
| 62 | `CRpyContentVersion` | Device→Host | Content version response |

**CRITICAL NOTE**: Earlier probes (nord_explore.c) used incorrect message type mappings — our msg_type=0 was intended as CReqBegin but was actually CQryPartList, and our msg_type=4 was intended as CQryPartList but was actually CReqBegin (without the required U32 payload). This corrupted the device firmware state and required a physical power cycle to recover.

### 8.3 UI Protocol

| Message | Direction | Description |
|---------|-----------|-------------|
| `CReqBegin` / `CAckBegin` | Req/Ack | Begin UI session |
| `CReqEnd` / `CAckEnd` | Req/Ack | End UI session |
| `CReqText` | Host→Device | Display text on device |
| `CQryInfo` / `CRpyInfo` | Qry/Rpy | Query device display info |
| `CReqProgress` | Host→Device | Update progress bar |

### 8.4 InstrCtrl Protocol

| Message | Direction | Description |
|---------|-----------|-------------|
| `CReqSetInstr` / `CAckSetInstr` | Req/Ack | Set active instrument |
| `CQryGetInstr` / `CRpyGetInstr` | Qry/Rpy | Get active instrument |
| `CReqSetInstrParam` / `CAckSetInstrParam` | Req/Ack | Set parameter |
| `CQryGetInstrParam` / `CRpyGetInstrParam` | Qry/Rpy | Get parameter |
| `CReqStrokeEnable` / `CAckStrokeEnable` | Req/Ack | Enable keystroke events |

### 8.5 MIDIX Protocol

| Message | Direction | Description |
|---------|-----------|-------------|
| `CReqNoteOn` / `CAckNoteOn` | Req/Ack | Play note |
| `CReqNoteOff` / `CAckNoteOff` | Req/Ack | Stop note |
| `CReqUpstreamEnable` / `CAckUpstreamEnable` | Req/Ack | Enable MIDI upstream |

---

## 9. Complete USB Init Sequence (from Disassembly)

This is the exact sequence Nord Sound Manager follows, reconstructed from arm64 disassembly with instruction addresses.

### Phase 1: IOKit Device Discovery

```
1. IOServiceMatching("IOUSBDevice")
2. IONotificationPortCreate(kIOMasterPortDefault)
3. CFRunLoopAddSource(runLoop, notifySource, kCFRunLoopDefaultMode)
4. IOServiceAddMatchingNotification(notifyPort, "IOServiceFirstMatch", matchDict, DeviceAddedCallback, ...)
5. IOServiceAddMatchingNotification(notifyPort, "IOServiceTerminate", matchDict, DeviceRemovedCallback, ...)
```

### Phase 2: Device Capability Probing (`QueryDeviceCapabilities` at `0x10003a594`)

```
1. IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, ...) → plugin
2. plugin->QueryInterface(kIOUSBDeviceInterfaceID182) → deviceInterface
3. deviceInterface->USBDeviceOpenSeize()
4. deviceInterface->GetDeviceVendor() → check == 0x0FFC
5. Read product name, manufacturer, serial via string descriptors
6. Vendor IN 0xC0/0x08 (4 bytes) → MaxBulkBufferSize
7. Vendor IN 0xC0/0x05 (2 bytes) → Build number
8. Vendor IN 0xC0/0x00 (2 bytes) → Model ID
9. Vendor IN 0xC0/0x04 (2 bytes) → Software version
10. Vendor IN 0xC1/0x09 (2 bytes, wIndex=ifNum) → Interface type (0=USB, 1=MIDI)
11. Enumerate interfaces, get endpoint count
```

### Phase 3: Open Device (`OpenDevice` at `0x10003b45c`)

```
1. IOCreatePlugInInterfaceForService → IOUSBDeviceInterface182
2. USBDeviceOpenSeize() — retries 12 times at 50ms intervals for exclusive access
3. CreateInterfaceIterator()
4. If no interfaces found:
   a. ResetDevice()                            ← CRITICAL
   b. GetConfigurationDescriptorPtr(0)
   c. SetConfiguration(configValue)            ← CRITICAL
   d. Retry CreateInterfaceIterator
5. OpenInterface():
   a. IOCreatePlugInInterfaceForService → IOUSBInterfaceInterface220
   b. USBInterfaceOpenSeize()                  ← Must use "Seize" variant
6. InitPipes(numEndpoints):
   For each pipe ref 1..N:
     GetPipeProperties(pipeRef, &dir, &num, &xferType, &maxPkt, &interval)
     if xferType == 3 (Interrupt): assign to interruptPipeRef
     elif dir == 0 (OUT): assign to bulkOutPipeRef
     elif dir == 1 (IN): assign to bulkInPipeRef
7. CreateInterfaceAsyncEventSource() → runLoopSource
8. MPCreateTask(ReadThreadFunction, runLoopSource) → DEDICATED THREAD for async I/O
```

### Phase 4: Start Async I/O

```
1. Start interrupt read: ReadPipeAsyncTO(interruptPipe, 16 bytes, timeout=0, callback)
2. Start bulk read: ReadPipeAsyncTO(bulkInPipe, 0xC000 bytes, timeout=0, callback)
   (0xC000 = 49152 bytes — the bulk read buffer size)
```

### Phase 5: Protocol Handshake

```
1. CCtrlProtocol::GetProtocolInfoAsync()
2. Build CQryProtocol message (Ctrl protocol, msg_type=2)
3. Frame: MsgProlog (size + protocol_id=0 + sub_id=0 + msg_type=2) + MsgEpilog (CRC-16)
4. SendAsync via WritePipeTO(bulkOutPipe, data, timeout=5000ms)
5. Start 60-second response timer
6. Device responds with CRpyProtocol on bulk IN
7. Parse protocol support map: for each protocol, record (id, version, supported)
8. Notify CPortManager::OnProtocolInfo(OK)
```

### Phase 6: Application Startup

```
1. Init_CreateProtocolManagers() — create handlers for each supported protocol
2. Init_StartupSequenceAsync()
3. Send UI Init → OnInitSent()
4. Send UI Begin → OnBeginSent()
5. GetPartitionStatesAndFileListsAsync():
   For each partition:
     CQryPartState → CRpyPartState
     CQryFileIterate → CRpyFileIterate (file listing)
6. OnPartitionListCompleted()
7. CFrmMain::OnReady() — app is fully connected
```

### Phase 7: File Transfer Session (for Move/Swap/Copy operations)

```
1. CReqBegin → CAckBegin (open session)
2. One or more of:
   - CReqFileMove(src, dst) → CAckFileMove
   - CReqFileSwap(a, b) → CAckFileSwap
   - CReqFileCopy(src, dst) → CAckFileCopy
   - CReqFileRename(spec, name) → CAckFileRename
   - CReqFileDelete(spec) → CAckFileDelete
3. CReqEnd → CAckEnd (close session)
```

---

## 10. Data Flow Through the Stack

### Sending a message (host → device)

```
Application:  CManager → CFileTransfer.SendAsyncTimeout(CMsgBase)
Protocol:     CMsgBase.Write(CBitStreamO) → serialize fields to bitstream
              CProtocolBase.SendAsync(CBitStreamO) → wrap with protocol framing
Port:         CPortUSB.SendAsync(CProtocolBase*, CBitStreamO)
                CPortUSBBase.MsgProlog() → [size placeholder][protocol_id][sub_id][msg_type]
                [message body — bit-packed]
                CPortUSBBase.MsgEpilog() → fill size field + append CRC-16
USB:          CStreamDuplex.WriteAsync(data, size)
                CUSBMan.WriteAsync(portId, data, size)
IOKit:        WritePipeTO(bulkOutPipe, data, size, timeout=5000ms)
```

### Receiving a message (device → host)

```
IOKit:        ReadPipeAsyncTO callback fires with data + bytesRead
USB:          CUSBMan::HandleBulkReadCompleted(portId, bytesRead)
Stream:       CStreamDuplex::BulkInputCallback → sends CIOEvent
Port:         CPortUSB::OnStreamBulkInput(CIOEvent)
              CPortUSB::HandleData(data, length)
              CPortUSBBase::OnReceive(CBitStreamI)
                Read total_size, validate
                Read CRC, validate against computed CRC
                Read protocol_id → dispatch to registered handler
                Read sub_id, msg_type → protocol-specific dispatch
Protocol:     CFileTransferBase::OnReceive → calls OnReply(CAckFileMove) etc.
Application:  CManager::OnQueueProcessed(EResult)
```

---

## 11. What's Been Tested (Probing Results)

### What Works

| Test | Result |
|------|--------|
| Find Nord via IOKit (`IOUSBHostDevice` matching VID/PID) | ✅ |
| Read USB configuration descriptor (131 bytes) | ✅ |
| All vendor control IN requests (0x00–0x0F) | ✅ Data received |
| Open vendor-specific interface with `USBInterfaceOpenSeize` | ✅ |
| Enumerate pipe properties | ✅ Correct assignment |
| `CreateInterfaceAsyncEventSource` + dedicated pthread | ✅ |
| `ReadPipeAsyncTO` (async bulk IN + interrupt IN) | ✅ |
| `WritePipeTO` (bulk OUT) | ✅ |
| **CQryProtocol handshake (proto=7, CRC-16/CCITT)** | ✅ **29-byte CRpyProtocol response received** |
| Synchronous `ReadPipe` / `ReadPipeTO` (bulk IN) | ❌ `0xe0004051` (kIOUSBPipeStalled) — use async only |

### Root Cause of Earlier Failures

All previous probes used `protocol_id = 0`, which maps to `CProtocolDummy` (a no-op handler). The correct Ctrl protocol ID is **7**, discovered via binary analysis of `CCtrlBase::GetProtocolID()`. The device silently discards messages addressed to protocol 0.

### Exhaustive Negative Results (from systematic probing)

A separate systematic probe tested every reasonable approach. These are **confirmed dead ends**:

| Approach | Result |
|----------|--------|
| USB SET_INTERFACE (alt=0, alt=1) | Alt 1 doesn't exist; alt 0 has no effect |
| USB SET_FEATURE / CLEAR_FEATURE on all endpoints | No effect on bulk stall |
| GET_STATUS on endpoints | Reports NOT halted — stall is firmware-generated |
| Vendor control OUT requests 0x00–0x20 with any wValue | All accepted, but registers are **read-only** — write-back has no effect |
| Echoing protocol version (0x0C04) back via vendor OUT | No effect |
| ClearPipeStall / ClearPipeStallBothEnds / AbortPipe / ResetPipe | All succeed but bulk IN still stalls |
| Writing various data on bulk OUT (zeros, protocol headers, SysEx, USB-MIDI, 64-byte packets) | All writes succeed but device silently discards; bulk IN remains stalled |
| Interrupt EP monitoring (EP 0x81) | Never has data, always times out |
| MIDI SysEx Identity Request via CoreMIDI | No response |
| Clavia-format SysEx (0x33 manufacturer ID) with 11+ model IDs | No response |
| MIDISendSysex (guaranteed delivery) | Completes but no response |
| The Nord only sends MIDI spontaneously when physically operated (keys/knobs/buttons) |

**Key conclusion**: The vendor interface bulk IN is firmware-inactive until the device receives a correctly framed protocol message. The vendor control registers are read-only status registers, not a command interface.

### Known Issues

1. **Synchronous bulk reads stall** — device requires async reads (`ReadPipeAsyncTO`). Sync reads return `kIOUSBPipeStalled`.
2. **Run loop threading** — Nord Sound Manager uses `MPCreateTask` to create a dedicated thread for the async run loop. Our single-threaded probe may not process callbacks correctly. The implementing agent MUST use a dedicated `pthread` for `CFRunLoopRun()`.
3. **Device firmware state persists across USB bus resets** — `ResetDevice()` only resets the bus layer, not the device firmware. If the firmware gets into a bad state (e.g., from receiving malformed FileTransfer messages), only a physical power cycle recovers it. `USBDeviceReEnumerate` and `USBDeviceSuspend` via IOKit also do NOT reset firmware state.
4. **Field size uncertainty** — the disassembly says `Wr_U32` for header fields, but `Wr_U32` in a bitstream library might mean `Write(value, 32_bits)`. If the actual protocol uses smaller fields (8-bit or 16-bit), the message would be shorter. **Confirmed**: 32-bit fields are correct based on working handshake.
5. **Protocol version in sub_protocol_id field is mandatory** — all messages after handshake must use the negotiated protocol version in the sub_protocol_id field (bytes 8–11). Using 0 (except for CQryProtocol) will cause the device to silently discard the message.

---

## 12. CQryProtocol — The First Message

This is the message that must be sent to establish communication. If this works, everything else follows.

### CONFIRMED Format (32-bit header fields, big-endian)

```
Offset  Bytes  Value         Description
0       4      00 00 00 12   total_size = 18 (4+4+4+4+2)
4       4      00 00 00 07   protocol_id = 7 (Ctrl)
8       4      00 00 00 00   sub_protocol_id = 0
12      4      00 00 00 02   message_type = 2 (CQryProtocol)
16      2      67 91         CRC-16/CCITT over bytes 0–15
```

**This exact 18-byte message was sent and produced a valid CRpyProtocol response (2026-03-22).**

### CRpyProtocol Response (captured from device)

```
Raw: 00 00 00 1D 00 00 00 07 00 00 00 00 00 00 00 03
     05 06 01 07 00 0A 02 0C 0A 0D 00 1C 14

Parsed:
  total_size = 29
  protocol_id = 7 (Ctrl)
  sub_protocol_id = 0
  message_type = 3 (CRpyProtocol)
  payload:
    count = 5
    Protocol 6 (UI): version 1
    Protocol 7 (Ctrl): version 0
    Protocol 10 (InstrCtrl): version 2
    Protocol 12 (FileTransfer): version 10
    Protocol 13 (MIDIX): version 0
  CRC = 0x1C14
```

### Implementation Notes (confirmed working)

1. Use a **dedicated pthread** for the CFRunLoop (required for async callbacks)
2. Start **interrupt read first**, then bulk read, then send the command
3. **Do NOT call ResetDevice** unless interfaces aren't found — unnecessary reset can confuse the device
4. Use `USBInterfaceOpenSeize` (the "Seize" variant is required)
5. WritePipeTO with noDataTimeout=1000ms, completionTimeout=5000ms
6. The device responds within ~100ms to valid messages

---

## 13. Prior Art & References

| Resource | URL | Relevance |
|----------|-----|-----------|
| ns3-program-viewer | https://github.com/Chris55/ns3-program-viewer | JS, reverse-engineered NS2/NS3 file formats |
| nord-documentation | https://chris55.github.io/nord-documentation/ | Binary offset docs for Nord files |
| g2ools | https://github.com/msg/g2ools | Python, full G2 USB protocol (CRC, framing, commands) |
| G2-Edit | https://github.com/chrispurusha/G2-Edit | C, G2 USB protocol with state machine |
| g2fx | https://github.com/sirlensalot/g2fx | Java, G2 USB protocol (most modern impl) |
| nord_g2_editor | https://github.com/BVerhue/nord_g2_editor | Pascal/Delphi, original G2 editor |
| Nordroid | https://github.com/Jurrie/Nordroid | Claims NE6D/Stage3 support, closed source |
| Snoize MIDI Monitor | https://www.snoize.com/MIDIMonitor/ | macOS MIDI spy tool (installed via `brew install --cask midi-monitor`) |
| Nord Electro 6 Manual | nordkeyboards.com | MIDI CC list, bank/program structure |
| Nord User Forum | norduserforum.com | Community workarounds for missing setlist mode |
| Linux HW DB | https://github.com/linuxhw/LsUSB | Nord Piano 4 USB descriptor dump |
| clavia.xml | `~/Library/Application Support/Nord Sound Manager/clavia.xml` | Full sound library catalog |
| sniffed_names.xml | Same directory | Piano/sample name mappings |

---

## 14. Partition Context — Critical Discovery (2026-03-22)

### The Problem

All `CQryFileIterate` calls returned status=2 (error) and all `CReqFileRead` calls returned status=3 (empty) despite correct payload formats confirmed via disassembly. The protocol was working at the session level but failing at the file level.

### Root Cause

**`CReqBegin` (msg_type=4) payload is the PARTITION ID**, not a generic session identifier. The device stores this in an internal `field_0x16f78` which gates ALL partition-specific operations. The existing `nord_ft_begin()` was sending partition_id=0 (Piano Native), so all Program partition queries operated in the wrong context.

### Confirmed from disassembly of `OnReply(CAckBegin)`:

```asm
; On success (status == 0):
ldr   w8, [x20, #0x14]           ; read partition_id from CAckBegin reply
str   w8, [x19, x9]              ; field_0x16f78 = partition_id

; On error:
str   w9=-1, [x19, x8]           ; field_0x16f78 = 0xFFFFFFFF (invalid)
```

### Correct Sequence Per Partition

```
1. CReqBegin(partId=4)       → CAckBegin(status=0, partId=4)
2. CQryPartState(4)          → CRpyPartState (optional, gets counts)
3. CQryFileIterate(0,0xFFFFFFFF,0) → loop: status=0 found, 1 skip, 2+ end
4. CQryFileInfo(partId, idx) → file metadata + name
5. CReqEnd                   → cleanup
```

### Wire Format

| Message | Payload |
|---------|---------|
| `CReqBegin` | `[U32 partition_id]` |
| `CAckBegin` | `[U32 status] [U32 partition_id]` (status 0=success) |
| `CQryFileIterate` | `[U32 start_index] [U32 category=0xFFFFFFFF] [U32 flags=0]` |
| `CRpyFileIterate` | `[U32 status] [U32 bank_index] [U32 active_slot]` |
| `CQryFileInfo` (8B) | `[U32 bank_index] [U32 slot_index]` — per-slot info (partId set by CReqBegin) |
| `CRpyFileInfo` | `[U32 status] [U32 ?] [U32 ?] [U32 size] [4B "ne6p"] [U32 ?] [U32 ?] [U32 category] [U32 name_len] [name_bytes...]` |
| `CReqFileOpen` (8B) | `[U32 bank_index] [U32 slot_index]` |
| `CAckFileOpen` | `[U32 status] [U32 file_size] [U32 ?]` |
| `CReqFileRead` (16B) | `[U32 bank_index] [U32 slot_index] [U32 offset] [U32 length]` |
| `CAckFileRead` | `[U32 status] [U32 bank] [U32 slot] [U32 offset] [U32 data_len] [data...]` |
| `CReqFileClose` (8B) | `[U32 bank_index] [U32 slot_index]` |
| `CQryFileGetDep` (8B) | `[U32 bank_index] [U32 slot_index]` |
| `CQryFileGetFocus` (4B) | `[U32 partition_id]` |
| `CRpyFileGetFocus` | `[U32 focused_bank_index]` |

### Management Operation Wire Formats (Confirmed 2026-03-22)

**CRITICAL**: `partId` is NOT sent in management payloads. The partition context is
established by `CReqBegin`. Only `bankIdx` and `slotIdx` are serialized on the wire.

| Message | Payload | Confirmed |
|---------|---------|-----------|
| `CReqFileCopy` (16B) | `[U32 srcBank] [U32 srcSlot] [U32 dstBank] [U32 dstSlot]` | status=0 ✓ |
| `CReqFileSwap` (16B) | `[U32 bank1] [U32 slot1] [U32 bank2] [U32 slot2]` | status=0 ✓ |
| `CReqFileMove` (16B) | `[U32 srcBank] [U32 srcSlot] [U32 dstBank] [U32 dstSlot]` | inferred (same as copy) |
| `CReqFileRename` (var) | `[U32 bankIdx] [U32 slotIdx] [U32 nameLen] [U8... name]` | status=0 ✓ |
| `CReqFileDelete` (8B) | `[U32 bankIdx] [U32 slotIdx]` | status=0 ✓ |
| `CAckFileCopy` (20B) | `[U32 status] [16B metadata]` | status 0=success |
| `CAckFileSwap` (4B) | `[U32 status]` | status 0=success |
| `CAckFileRename` (4B) | `[U32 status]` | status 0=success |
| `CAckFileDelete` (4B) | `[U32 status]` | status 0=success |

**Notes on copy behavior**: Copying a user bank (with ne6p type) creates a new bank that
initially appears as "factory" in FileInfo queries (no ne6p marker). The program data is
present and functional. The rename operation returns success but the name may not appear
in FileInfo until a fresh partition session.

### Iterator Semantics

- `start_index` = bank index (0–25 for A–Z)
- status=0: bank has content, `f1`=bank_index, `f2`=active_slot_within_bank
- status=1: bank empty, `f2`=0xFFFFFFFF
- status=2+: end of iteration
- Advance: `start_index = f1 + 1`
- Each "file" represents a BANK (not an individual program slot)

### Per-Slot Access — CORRECTED (2026-03-22)

**CRITICAL DISCOVERY**: Inside a `CReqBegin(partId)` session, ALL file operations use
`[bankIdx][slotIdx]` payloads — **NOT** `[partId][bankIdx]`. The partition context is
established by CReqBegin and does NOT appear in subsequent payloads.

Earlier probes accidentally sent `partId=4` as the bank index, which shifted all queries
to Bank E (index 4). This caused all per-slot names to appear identical (they were all
querying the same bank) and CReqFileRead to return status=3 ("Busy").

**Correct per-slot addressing:**
- `CQryFileInfo(bankIdx, slotIdx)` — returns per-program name, size, type, category
- `CReqFileOpen(bankIdx, slotIdx)` → `CReqFileRead(bankIdx, slotIdx, 0, 255)` — reads program data
- `CReqFileClose(bankIdx, slotIdx)` — closes file handle
- Each program has its OWN name (e.g., A:1:1 = "-Vinyl Biscuit-", A:1:2 = "ComeTogether5Gm")
- Slot addressing: 0-15 maps to Page 1-4, Program 1-4 (slot = (page-1)*4 + (prog-1))
- Returns status=1 for empty/factory-only slots

### File Architecture

Each bank (A-Z) contains up to 16 individually-named program slots:
- Banks with `ne6p` type in FileInfo = user-saved content
- Banks without `ne6p` type = factory/ROM presets
- `CReqFileRead(bank, slot, 0, 255)` returns 255 bytes of raw program data (CONFIRMED WORKING)
- `CQryFileGetDep(bank, slot)` returns sample dependencies (piano names, sample library names)
- `CQryBankList(partId)` wire format:
  ```
  Response: [U32 status][U32 partId][U8 bankCount]
  Per bank: [U32 nameLen][nameBytes...][U32 slotsPerBank]

  Example (NE6, partition 4):
    status=0, partId=4, bankCount=26
    Bank "Bank A", slotsPerBank=16
    Bank "Bank B", slotsPerBank=16
    ... (26 entries, 373 total bytes)
  ```
- .ne6p file format: 44-byte CBIN header + 211 bytes program data; bank index at byte 0x0C, slot index at byte 0x0E

### Status Codes (Corrected from NSM binary analysis)

| Code | Meaning |
|------|---------|
| 0 | Success |
| 1 | Not found / empty slot |
| 3 | Busy (often caused by wrong payload format) |
| 4 | File error |
| 5 | File exists |
| 22 | Not supported |

### NE6 Partitions

| ID | Name | Banks | Content |
|----|------|-------|---------|
| 0 | Piano (Native) | 21 | Built-in piano samples |
| 1 | Piano | 21 | User piano samples |
| 2 | Samp Lib (Native) | 271 | Built-in sample library |
| 3 | Samp Lib | 271 | User sample library |
| 4 | Program | 26 (A-Z) | **Program presets (main target)** |
| 5 | Live | 8 | Live performance presets |
| 6 | Settings | 1 | Device settings |

---

## 15. Implementation — `nord` CLI

### Phase 2: Core CLI (COMPLETE)

C CLI using IOKit direct USB access (`_tools/nord-cli/nord_cli.c`):

```
nord status                          # Connection info, firmware version, partition summary
nord list [-d] [-a] [-p N]          # List all banks (-d=deps, -a=empty, -p=partition)
nord list A [-d]                    # List all 16 slots in bank A (page:prog format)
nord copy A:1:3 F:2:1 [-p N]       # Copy program to destination
nord move A:1:3 F:2:1 [-p N]       # Move program (source becomes empty)
nord swap A:1:1 F:2:1 [-p N]       # Swap two programs
nord rename A:1:3 "Name" [-p N]    # Rename program
nord delete F:2:1 [-p N]           # Delete program
```

Build: `clang -framework IOKit -framework CoreFoundation -o nord nord_usb.c nord_cli.c -Wno-deprecated-declarations`

Address format: `BANK:PAGE:PROG` (e.g. `A:1:3`, `F:2:1`). Matches `nord list` output and the physical keyboard layout. Just `A` defaults to `A:1:1`.

### Phase 3: Claude Code Skill (NEXT)

Claude Code skill (`/nord-setlist`) that exposes the CLI operations as tools.
The LLM handles setlist matching, arrangement strategy, and multi-step operations
conversationally — no bespoke setlist engine needed since the CLI primitives
(list/copy/move/swap/rename/delete) are sufficient building blocks.
