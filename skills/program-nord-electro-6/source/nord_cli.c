/*
 * nord_cli.c — Nord Electro 6 USB CLI tool
 *
 * Usage:
 *   nord list [-d] [-a] [-p N]       List programs (default: partition 4)
 *   nord status                       Show connection info
 *   nord copy A:1:3 F:2:1            Copy program to destination
 *   nord move A:1:3 F:2:1            Move program to destination
 *   nord swap A:1:3 F:2:1            Swap two programs
 *   nord rename A:1:3 "Name"         Rename a program
 *   nord delete F:2:1                Delete a program
 *
 * Address format: BANK:PAGE:PROG (e.g. A:1:3 = bank A, page 1, prog 3)
 *   Matches the output of `nord list` and the physical keyboard layout.
 *   Legacy format BANK:SLOT (e.g. A:3) is accepted with a deprecation warning.
 *
 * Build:
 *   clang -framework IOKit -framework CoreFoundation -o nord \
 *     nord_usb.c nord_cli.c -Wno-deprecated-declarations
 */

#include "nord_usb.h"
#include "nord_names.h"
#include "nord_addr.h"
#include "nord_planner.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// ─── Bank info ───

typedef struct {
    int bankIdx;
    int activeSlot;
    bool hasContent;
    bool hasNe6p;
    char name[256];
    int depCount;
    char deps[4][256];
} BankInfo;

static void get_bank_info(NordDevice *nd, int bankIdx, int slotIdx, BankInfo *b) {
    memset(b, 0, sizeof(BankInfo));
    b->bankIdx = bankIdx;
    b->activeSlot = -1;

    uint8_t resp[4096];
    int len = nord_ft_file_info(nd, bankIdx, slotIdx, resp, sizeof(resp));
    if (len <= 0) return;

    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;

    // Detect user program by checking for any Nord program type marker
    // (e.g. "ne6p" for Electro 6, "ns3p" for Stage 3, "np4p" for Piano 4)
    b->hasNe6p = (pLen >= 20 && d[19] == 'p' &&
                  d[16] >= 'a' && d[16] <= 'z');

    if (pLen >= 37) {
        uint32_t nameLen = nord_get_be32(d + 32);
        if (nameLen > 0 && nameLen < 200 && (int)(36 + nameLen) <= pLen) {
            int n = nameLen < 255 ? (int)nameLen : 255;
            memcpy(b->name, d + 36, n);
            b->name[n] = '\0';
            for (int k = 0; k < n; k++)
                if ((unsigned char)b->name[k] < 0x20 || (unsigned char)b->name[k] > 0x7E)
                    b->name[k] = '\0';
        }
    }
}

static void get_bank_deps(NordDevice *nd, int bankIdx, int slotIdx, BankInfo *b) {
    uint8_t resp[4096];
    int len = nord_ft_get_dep(nd, bankIdx, slotIdx, resp, sizeof(resp));
    if (len <= 0) return;

    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;
    if (pLen < 16) return;

    b->depCount = 0;
    int pos = 16;
    while (pos < pLen && b->depCount < 4) {
        if (d[pos] >= 0x04 && d[pos] <= 0x40 && pos + 1 + d[pos] <= pLen) {
            int nl = d[pos];
            bool allPrint = true;
            for (int j = 1; j <= nl; j++) {
                if (d[pos + j] < 0x20 || d[pos + j] > 0x7E) { allPrint = false; break; }
            }
            if (allPrint && nl >= 4) {
                int n = nl < 255 ? nl : 255;
                memcpy(b->deps[b->depCount], d + pos + 1, n);
                b->deps[b->depCount][n] = '\0';
                b->depCount++;
                pos += nl + 1;
                continue;
            }
        }
        pos++;
    }
}

// ─── Connect helper ───

static NordDevice *connect_nord(void) {
    NordDevice *nd = nord_open();
    if (!nd) { fprintf(stderr, "Error: No Nord keyboard found\n"); return NULL; }
    if (!nord_handshake(nd)) {
        fprintf(stderr, "Error: Protocol handshake failed\n");
        nord_close(nd);
        return NULL;
    }
    nord_ui_begin(nd);
    return nd;
}

static void disconnect_nord(NordDevice *nd) {
    nord_ui_end(nd);
    nord_close(nd);
}

// ─── Commands ───

static int cmd_status(NordDevice *nd) {
    printf("%s — Connected\n", nd->productName);
    printf("  Product ID: 0x%04X\n", nd->productId);
    printf("  Firmware: %d.%02d (build %d)\n",
           nd->fwVersion >> 8, nd->fwVersion & 0xFF, nd->fwBuild);

    // Query memory layout for Program partition
    if (nord_query_layout(nd, 4)) {
        printf("  Memory: %d banks x %d slots (%d programs)\n",
               nd->numBanks, nd->slotsPerBank,
               nd->numBanks * nd->slotsPerBank);
    }
    printf("  Max bulk buffer: %u bytes\n", nd->maxBulkBuf);
    printf("  Protocols:");
    for (int i = 0; i < 16; i++) {
        if (nd->protocols[i].supported)
            printf(" %d(v%d)", i, nd->protocols[i].version);
    }
    printf("\n");

    // Show partition summary
    static const char *partNames[] = {
        "Piano (Native)", "Piano", "Samp Lib (Native)", "Samp Lib",
        "Program", "Live", "Settings"
    };
    printf("\n  Partitions:\n");
    for (int p = 0; p <= 6; p++) {
        if (!nord_ft_begin_partition(nd, p)) continue;

        int count = 0;
        uint32_t startIdx = 0;
        for (int i = 0; i < 300; i++) {
            uint8_t resp[256];
            int len = nord_ft_iterate(nd, startIdx, resp, sizeof(resp));
            if (len <= 0) break;
            uint8_t *d = resp + NORD_HEADER_SIZE;
            uint32_t status = nord_get_be32(d);
            uint32_t f1 = nord_get_be32(d + 4);
            if (status >= 2) break;
            if (status == 0) count++;
            startIdx = f1 + 1;
        }
        printf("    [%d] %-20s %d populated\n", p, partNames[p], count);
        nord_ft_end(nd);
    }

    return 0;
}

// Per-slot info from FileInfo response
typedef struct {
    char name[256];
    uint32_t hash;
    uint32_t size;
    uint32_t category;
    bool hasNe6p;
    bool ok;
} SlotDetail;

// Get per-slot FileInfo using [bankIdx][slotIdx] (partition set by CReqBegin)
static bool get_slot_info(NordDevice *nd, int bankIdx, int slotIdx,
                          SlotDetail *s) {
    memset(s, 0, sizeof(*s));
    uint8_t pay[8], resp[4096];
    nord_put_be32(pay, bankIdx);
    nord_put_be32(pay + 4, slotIdx);
    int len = nord_ft_send(nd, MSG_FT_QRY_FILE_INFO, pay, 8, resp, sizeof(resp), 5000);
    if (len <= 0) return false;

    int pLen = len - NORD_HEADER_SIZE - NORD_CRC_SIZE;
    uint8_t *d = resp + NORD_HEADER_SIZE;
    uint32_t status = nord_get_be32(d);
    if (status != 0) return false;

    s->ok = true;
    s->size = (pLen >= 16) ? nord_get_be32(d + 12) : 0;
    s->hasNe6p = (pLen >= 20 && d[19] == 'p' && d[16] >= 'a' && d[16] <= 'z');
    s->category = (pLen >= 32) ? nord_get_be32(d + 28) : 0;
    if (pLen >= 37) {
        uint32_t nameLen = nord_get_be32(d + 32);
        if (nameLen > 0 && nameLen < 200 && (int)(36 + nameLen) <= pLen) {
            int n = nameLen < 255 ? (int)nameLen : 255;
            memcpy(s->name, d + 36, n);
            s->name[n] = '\0';
            for (int k = 0; k < n; k++)
                if ((unsigned char)s->name[k] < 0x20 || (unsigned char)s->name[k] > 0x7E)
                    s->name[k] = '\0';
            int hashOff = 36 + (int)nameLen;
            if (hashOff + 4 <= pLen)
                s->hash = nord_get_be32(d + hashOff);
        }
    }
    return true;
}

// List all 16 slots within a specific bank
// Print the per-slot detail for one bank. Caller must have an active partition.
// activeSlot may be -1 to skip the active marker.
static void print_bank_detail(NordDevice *nd, int bankIdx, int activeSlot,
                              bool showDeps, bool verbose) {
    printf("%s — Bank %c\n\n", nd->productName, 'A' + bankIdx);

    BankInfo bankInfo;
    memset(&bankInfo, 0, sizeof(bankInfo));
    if (showDeps || verbose) {
        get_bank_info(nd, bankIdx, 0, &bankInfo);
        if (bankInfo.hasNe6p)
            get_bank_deps(nd, bankIdx, 0, &bankInfo);
    }

    SlotDetail slots[16];
    uint32_t seenSizes[16];
    int nSeen = 0;

    for (int slot = 0; slot < 16; slot++) {
        get_slot_info(nd, bankIdx, slot, &slots[slot]);

        if (slots[slot].ok) {
            bool found = false;
            for (int k = 0; k < nSeen; k++)
                if (seenSizes[k] == slots[slot].size) { found = true; break; }
            if (!found) seenSizes[nSeen++] = slots[slot].size;
        }

        int page = slot / 4 + 1;
        int prog = slot % 4 + 1;
        bool isActive = (slot == activeSlot);

        printf("  %s %c:%d:%d", isActive ? ">" : " ", 'A' + bankIdx, page, prog);

        if (!slots[slot].ok) {
            printf("  (empty)\n");
            continue;
        }

        if (slots[slot].hasNe6p && slots[slot].name[0]) {
            printf("  \"%s\"", slots[slot].name);
        } else if (!slots[slot].hasNe6p) {
            printf("  (factory)");
        }

        if (verbose)
            printf("  [%u bytes, cat=%u]", slots[slot].size, slots[slot].category);

        printf("\n");
    }

    if ((showDeps || verbose) && bankInfo.depCount > 0) {
        printf("\n  Samples:\n");
        for (int j = 0; j < bankInfo.depCount; j++)
            printf("    -> %s\n", bankInfo.deps[j]);
    }

    if (verbose) {
        printf("\n  %d unique program size%s across 16 slots\n",
               nSeen, nSeen == 1 ? "" : "s");
    }

    if (activeSlot >= 0) {
        printf("  Active: %c:%d:%d\n",
               'A' + bankIdx, activeSlot / 4 + 1, activeSlot % 4 + 1);
    }
}

static int cmd_list_bank(NordDevice *nd, int partId, int bankIdx,
                         bool showDeps, bool verbose) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    int activeSlot = -1;
    {
        uint8_t resp[4096];
        int len = nord_ft_iterate(nd, bankIdx, resp, sizeof(resp));
        if (len > 0) {
            uint8_t *d = resp + NORD_HEADER_SIZE;
            uint32_t status = nord_get_be32(d);
            uint32_t f1 = nord_get_be32(d + 4);
            uint32_t f2 = nord_get_be32(d + 8);
            if (status == 0 && f1 == (uint32_t)bankIdx)
                activeSlot = (f2 == 0xFFFFFFFF) ? -1 : (int)f2;
        }
    }

    print_bank_detail(nd, bankIdx, activeSlot, showDeps, verbose);

    nord_ft_end(nd);
    return 0;
}

static int cmd_list(NordDevice *nd, int partId, bool showDeps, bool showEmpty,
                    bool verbose, bool listAll, int filterBank) {
    // If a specific bank was requested, show per-slot detail
    if (filterBank >= 0)
        return cmd_list_bank(nd, partId, filterBank, showDeps, verbose);

    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    int focusBank = nord_ft_get_focus(nd, partId);

    static const char *partNames[] = {
        "Piano (Native)", "Piano", "Samp Lib (Native)", "Samp Lib",
        "Program", "Live", "Settings"
    };
    const char *partName = (partId >= 0 && partId <= 6) ? partNames[partId] : "Unknown";
    printf("%s — %s (partition %d)\n\n", nd->productName, partName, partId);

    int populated = 0, total = 0;
    uint32_t startIdx = 0;

    for (int i = 0; i < 300; i++) {
        uint8_t resp[4096];
        int len = nord_ft_iterate(nd, startIdx, resp, sizeof(resp));
        if (len <= 0) break;

        uint8_t *d = resp + NORD_HEADER_SIZE;
        uint32_t status = nord_get_be32(d);
        uint32_t f1 = nord_get_be32(d + 4);
        uint32_t f2 = nord_get_be32(d + 8);

        if (status >= 2) break;

        total++;
        bool hasContent = (status == 0);
        if (hasContent) populated++;

        // Only display banks A-Z (indices 0-25)
        if (f1 > 25) { startIdx = f1 + 1; continue; }

        if (hasContent || showEmpty) {
            if (listAll && hasContent) {
                int activeSlot = (f2 == 0xFFFFFFFF) ? -1 : (int)f2;
                print_bank_detail(nd, (int)f1, activeSlot, showDeps, verbose);
                printf("\n");
            } else {
                BankInfo b;
                memset(&b, 0, sizeof(b));
                b.bankIdx = f1;
                b.activeSlot = (f2 == 0xFFFFFFFF) ? -1 : (int)f2;
                b.hasContent = hasContent;

                if (hasContent) {
                    int activeSlot = (f2 == 0xFFFFFFFF) ? 0 : (int)f2;
                    get_bank_info(nd, f1, activeSlot, &b);
                    b.activeSlot = (f2 == 0xFFFFFFFF) ? -1 : (int)f2;
                    if (showDeps && b.hasNe6p)
                        get_bank_deps(nd, f1, activeSlot, &b);
                }

                bool isFocus = ((int)f1 == focusBank);

                // Show as page:prog format
                int page = (b.activeSlot >= 0) ? b.activeSlot / 4 + 1 : 0;
                int prog = (b.activeSlot >= 0) ? b.activeSlot % 4 + 1 : 0;
                char loc[12];
                if (b.activeSlot >= 0)
                    snprintf(loc, sizeof(loc), "%c:%d:%d", 'A' + (int)f1, page, prog);
                else
                    snprintf(loc, sizeof(loc), "%c", 'A' + (int)f1);

                printf("  %s %-6s", isFocus ? ">" : " ", loc);

                if (b.hasNe6p && b.name[0])
                    printf("  \"%s\"", b.name);

                if (!hasContent) printf("  (empty)");
                else if (!b.hasNe6p) printf("  (factory)");

                printf("\n");

                if (showDeps && b.depCount > 0) {
                    for (int j = 0; j < b.depCount; j++)
                        printf("         -> %s\n", b.deps[j]);
                }
            }
        }

        startIdx = f1 + 1;
    }

    printf("\n  %d populated / %d total banks\n", populated, total);
    if (focusBank >= 0 && focusBank < 26)
        printf("  Active: Bank %c\n", 'A' + focusBank);

    nord_ft_end(nd);
    return 0;
}

// ─── Name collection (for uniqueness checks) ───

typedef struct {
    char names[26][256];
    const char *ptrs[26];  // pointers into names[][] for nord_name_conflicts()
    int count;
} BankNameList;

// Collect all bank names via iterator + FileInfo. Caller must have an active partition.
// skip_bank is excluded from collection (-1 to collect all).
static void collect_bank_names(NordDevice *nd, int skip_bank, BankNameList *list) {
    memset(list, 0, sizeof(*list));

    uint32_t startIdx = 0;
    for (int i = 0; i < 300; i++) {
        uint8_t resp[4096];
        int len = nord_ft_iterate(nd, startIdx, resp, sizeof(resp));
        if (len <= 0) break;

        uint8_t *d = resp + NORD_HEADER_SIZE;
        uint32_t status = nord_get_be32(d);
        uint32_t f1 = nord_get_be32(d + 4);

        if (status >= 2) break;
        if (status == 0 && (int)f1 <= 25 && (int)f1 != skip_bank) {
            BankInfo b;
            get_bank_info(nd, f1, 0, &b);
            if (b.name[0]) {
                strncpy(list->names[list->count], b.name, 255);
                list->names[list->count][255] = '\0';
                list->ptrs[list->count] = list->names[list->count];
                list->count++;
            }
        }
        startIdx = f1 + 1;
    }
}

static int cmd_copy(NordDevice *nd, int partId, BankSpec *src, BankSpec *dst) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    char srcAddr[16], dstAddr[16];
    format_bank_spec(srcAddr, sizeof(srcAddr), src->bank, src->slot);
    format_bank_spec(dstAddr, sizeof(dstAddr), dst->bank, dst->slot);
    printf("Copy %s -> %s ... ", srcAddr, dstAddr);
    fflush(stdout);

    int status = nord_ft_copy(nd, src->bank, src->slot, dst->bank, dst->slot);
    if (status == 0) {
        printf("OK");
        // Report the actual name assigned (firmware may auto-suffix for uniqueness)
        BankInfo dstInfo;
        get_bank_info(nd, dst->bank, dst->slot, &dstInfo);
        if (dstInfo.name[0])
            printf(" -> \"%s\"", dstInfo.name);
        printf("\n");
    } else if (status == 4) {
        printf("FAILED (destination not empty)\n");
    } else {
        printf("FAILED (status=%d)\n", status);
    }

    nord_ft_end(nd);
    return status != 0;
}

static int cmd_move(NordDevice *nd, int partId, BankSpec *src, BankSpec *dst) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    char srcAddr[16], dstAddr[16];
    format_bank_spec(srcAddr, sizeof(srcAddr), src->bank, src->slot);
    format_bank_spec(dstAddr, sizeof(dstAddr), dst->bank, dst->slot);
    printf("Move %s -> %s ... ", srcAddr, dstAddr);
    fflush(stdout);

    int status = nord_ft_move(nd, src->bank, src->slot, dst->bank, dst->slot);
    if (status == 0)
        printf("OK\n");
    else
        printf("FAILED (status=%d)\n", status);

    nord_ft_end(nd);
    return status != 0;
}

static int cmd_swap(NordDevice *nd, int partId, BankSpec *a, BankSpec *b) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    char aAddr[16], bAddr[16];
    format_bank_spec(aAddr, sizeof(aAddr), a->bank, a->slot);
    format_bank_spec(bAddr, sizeof(bAddr), b->bank, b->slot);
    printf("Swap %s <-> %s ... ", aAddr, bAddr);
    fflush(stdout);

    int status = nord_ft_swap(nd, a->bank, a->slot, b->bank, b->slot);
    if (status == 0)
        printf("OK\n");
    else
        printf("FAILED (status=%d)\n", status);

    nord_ft_end(nd);
    return status != 0;
}

static int cmd_rename(NordDevice *nd, int partId, BankSpec *spec, const char *name) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    // Validate name for firmware compatibility
    int badIdx = nord_name_validate(name);
    if (badIdx >= 0) {
        int len = (int)strlen(name);
        if (badIdx == len) {
            fprintf(stderr, "FAILED (name too long: %d chars, max %d)\n",
                    len, NORD_NAME_MAX_LEN);
        } else {
            fprintf(stderr, "FAILED (invalid character '%c' at position %d"
                    " — only A-Z, a-z, 0-9, space, and hyphen allowed)\n",
                    name[badIdx], badIdx);
        }
        nord_ft_end(nd);
        return 1;
    }

    // Check for name uniqueness before renaming
    BankNameList nameList;
    collect_bank_names(nd, spec->bank, &nameList);
    int conflict = nord_name_conflicts(name, nameList.ptrs, nameList.count, -1);
    if (conflict >= 0) {
        printf("FAILED (name \"%s\" already in use)\n", name);
        char suggestion[256];
        int sfx = nord_make_unique(name, nameList.ptrs, nameList.count,
                                   -1, suggestion, sizeof(suggestion));
        if (sfx > 0)
            printf("  Suggestion: \"%s\"\n", suggestion);
        nord_ft_end(nd);
        return 1;
    }

    char addr[16];
    format_bank_spec(addr, sizeof(addr), spec->bank, spec->slot);
    printf("Rename %s -> \"%s\" ... ", addr, name);
    fflush(stdout);

    int status = nord_ft_rename(nd, spec->bank, spec->slot, name);
    if (status == 0)
        printf("OK\n");
    else
        printf("FAILED (status=%d)\n", status);

    nord_ft_end(nd);
    return status != 0;
}

static int cmd_delete(NordDevice *nd, int partId, BankSpec *spec) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    char addr[16];
    format_bank_spec(addr, sizeof(addr), spec->bank, spec->slot);
    printf("Delete %s ... ", addr);
    fflush(stdout);

    int status = nord_ft_delete(nd, spec->bank, spec->slot);
    if (status == 0)
        printf("OK\n");
    else if (status == 1)
        printf("OK (already empty)\n");
    else
        printf("FAILED (status=%d)\n", status);

    nord_ft_end(nd);
    return (status != 0 && status != 1);  // 1 = already empty = OK
}

static int cmd_export(NordDevice *nd, int partId, BankSpec *spec,
                      const char *filename) {
    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        return 1;
    }

    char addr[16];
    format_bank_spec(addr, sizeof(addr), spec->bank, spec->slot);

    // Get file size from FileInfo
    SlotDetail slotInfo;
    if (!get_slot_info(nd, spec->bank, spec->slot, &slotInfo) || !slotInfo.ok) {
        fprintf(stderr, "Error: %s is empty or not readable\n", addr);
        nord_ft_end(nd);
        return 1;
    }
    uint32_t fileSize = slotInfo.size;

    // Open file on device
    int status = nord_ft_file_open(nd, spec->bank, spec->slot, NULL);
    if (status != 0) {
        fprintf(stderr, "Error: Cannot open %s (status=%d)\n", addr, status);
        nord_ft_end(nd);
        return 1;
    }

    printf("Export %s (%u bytes) -> %s ... ", addr, fileSize, filename);
    fflush(stdout);

    // Read file data in chunks
    uint8_t *data = malloc(fileSize);
    if (!data) {
        fprintf(stderr, "FAILED (out of memory)\n");
        nord_ft_file_close(nd, spec->bank, spec->slot);
        nord_ft_end(nd);
        return 1;
    }

    uint32_t totalRead = 0;
    int ret = 0;
    while (totalRead < fileSize) {
        uint32_t chunkSize = fileSize - totalRead;
        if (chunkSize > 4096) chunkSize = 4096;
        uint32_t bytesRead = 0;
        status = nord_ft_file_read(nd, spec->bank, spec->slot,
                                    totalRead, chunkSize,
                                    data + totalRead, chunkSize, &bytesRead);
        if (status != 0 || bytesRead == 0) {
            fprintf(stderr, "FAILED (read error at offset %u, status=%d)\n",
                    totalRead, status);
            ret = 1;
            break;
        }
        totalRead += bytesRead;
    }

    nord_ft_file_close(nd, spec->bank, spec->slot);
    nord_ft_end(nd);

    if (ret == 0) {
        // Write to disk
        FILE *f = fopen(filename, "wb");
        if (!f) {
            fprintf(stderr, "FAILED (cannot write %s)\n", filename);
            ret = 1;
        } else {
            fwrite(data, 1, totalRead, f);
            fclose(f);
            printf("OK (%u bytes)\n", totalRead);
        }
    }

    free(data);
    return ret;
}

static int cmd_import(NordDevice *nd, int partId, const char *filename,
                      BankSpec *spec) {
    // Read file from disk
    FILE *f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot read %s\n", filename);
        return 1;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsize <= 0 || fsize > 1024 * 1024) {
        fprintf(stderr, "Error: Invalid file size (%ld bytes)\n", fsize);
        fclose(f);
        return 1;
    }
    uint8_t *data = malloc(fsize);
    if (!data) { fclose(f); return 1; }
    fread(data, 1, fsize, f);
    fclose(f);

    // Detect and parse CBIN header if present
    uint8_t *progData = data;
    long progSize = fsize;
    uint32_t fileType = 0;
    uint32_t category = 0xFFFFFFFF;
    bool hasCBIN = (fsize >= 48 && data[0] == 'C' && data[1] == 'B' &&
                    data[2] == 'I' && data[3] == 'N');
    if (hasCBIN) {
        // Extract metadata from CBIN header (44 bytes)
        // Offset 0x10: category (4 bytes, LE)
        // fileType is the 4-char type string packed as big-endian U32
        // (confirmed from CFileType::Type2Ext disassembly — extracts bytes MSB first)
        // "ne6p" → 0x6E653670
        fileType = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
        category = data[16] | (data[17] << 8) | (data[18] << 16) | (data[19] << 24);
        progData = data + 44;  // skip 44-byte CBIN header
        progSize = fsize - 44;
    }

    if (!nord_ft_begin_partition(nd, partId)) {
        fprintf(stderr, "Error: Failed to select partition %d\n", partId);
        free(data);
        return 1;
    }

    char addr[16];
    format_bank_spec(addr, sizeof(addr), spec->bank, spec->slot);
    printf("Import %s -> %s (%ld bytes%s) ... ",
           filename, addr, progSize, hasCBIN ? ", CBIN header stripped" : "");
    fflush(stdout);

    // Extract a name from the filename (strip path and extension)
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    char progName[129];
    strncpy(progName, base, 128);
    progName[128] = '\0';
    char *dot = strrchr(progName, '.');
    if (dot) *dot = '\0';

    // Create file on device
    int status = nord_ft_file_create(nd, spec->bank, spec->slot,
                                      (uint32_t)progSize, fileType, category, progName);
    if (status != 0) {
        printf("FAILED (create error, status=%d — slot may not be empty)\n", status);
        nord_ft_end(nd);
        free(data);
        return 1;
    }

    // Write raw program data directly after create (no open needed)
    uint32_t totalWritten = 0;
    int ret = 0;
    while (totalWritten < (uint32_t)progSize) {
        uint32_t chunkSize = (uint32_t)progSize - totalWritten;
        if (chunkSize > 4096) chunkSize = 4096;
        status = nord_ft_file_write(nd, spec->bank, spec->slot,
                                     totalWritten, progData + totalWritten, chunkSize);
        if (status != 0) {
            fprintf(stderr, "[write] status=%d at offset %u\n", status, totalWritten);
            printf("FAILED (write error)\n");
            ret = 1;
            break;
        }
        totalWritten += chunkSize;
    }

    // Close the file
    status = nord_ft_file_close(nd, spec->bank, spec->slot);
    if (ret == 0 && status == 0) {
        printf("OK (%u bytes written)\n", totalWritten);
    } else if (ret == 0) {
        printf("FAILED (close error, status=%d)\n", status);
        ret = 1;
    }

    if (ret != 0) {
        // Clean up partially created file
        nord_ft_delete(nd, spec->bank, spec->slot);
    }

    nord_ft_end(nd);
    free(data);
    return ret;
}

// ─── Plan (offline; no hardware) ───

static int cmd_plan(const char *beforePath, const char *afterPath, bool jsonOut) {
    if (!beforePath || !afterPath) {
        fprintf(stderr, "Usage: nord plan --before FILE --after FILE [--json]\n");
        return 1;
    }

    FILE *fb = fopen(beforePath, "r");
    if (!fb) { fprintf(stderr, "Cannot open --before %s\n", beforePath); return 1; }
    FILE *fa = fopen(afterPath, "r");
    if (!fa) { fprintf(stderr, "Cannot open --after %s\n", afterPath); fclose(fb); return 1; }

    NordState *before = calloc(1, sizeof(NordState));
    NordState *after = calloc(1, sizeof(NordState));
    if (!before || !after) {
        fprintf(stderr, "Out of memory\n");
        free(before); free(after); fclose(fa); fclose(fb);
        return 1;
    }

    char err[256] = {0};
    if (!nord_plan_load_state(fb, before, err, sizeof(err))) {
        fprintf(stderr, "Error reading --before: %s\n", err);
        fclose(fb); fclose(fa); free(before); free(after);
        return 1;
    }
    fclose(fb);
    if (!nord_plan_load_state(fa, after, err, sizeof(err))) {
        fprintf(stderr, "Error reading --after: %s\n", err);
        fclose(fa); free(before); free(after);
        return 1;
    }
    fclose(fa);

    NordPlan *plan = calloc(1, sizeof(NordPlan));
    if (!plan) {
        fprintf(stderr, "Out of memory\n");
        free(before); free(after);
        return 1;
    }
    nord_plan_compute(before, after, plan);

    int ret = 0;
    if (plan->error != NORD_PLAN_OK) {
        fprintf(stderr, "Plan error: %s\n", plan->errorDetail);
        ret = 1;
    } else if (jsonOut) {
        printf("[");
        for (int i = 0; i < plan->count; i++) {
            const NordPlanOp *op = &plan->ops[i];
            char a[16], b[16];
            format_bank_spec(a, sizeof(a), op->a.bank, op->a.slot);
            format_bank_spec(b, sizeof(b), op->b.bank, op->b.slot);
            if (i > 0) printf(",");
            printf("\n  {\"op\":\"%s\",\"a\":\"%s\",\"b\":\"%s\"}",
                   op->kind == NORD_OP_MOVE ? "move" : "swap", a, b);
        }
        printf("%s]\n", plan->count ? "\n" : "");
    } else {
        for (int i = 0; i < plan->count; i++) {
            const NordPlanOp *op = &plan->ops[i];
            char a[16], b[16];
            format_bank_spec(a, sizeof(a), op->a.bank, op->a.slot);
            format_bank_spec(b, sizeof(b), op->b.bank, op->b.slot);
            printf("nord %s %s %s\n",
                   op->kind == NORD_OP_MOVE ? "move" : "swap", a, b);
        }
    }

    free(before);
    free(after);
    free(plan);
    return ret;
}

// ─── Usage ───

static void usage(void) {
    printf("nord — Nord USB CLI\n\n");
    printf("Usage:\n");
    printf("  nord list [-d] [-a] [-v] [-p N]   List all banks (one line each)\n");
    printf("  nord list --all [-d] [-v] [-p N]  List all banks with full slot detail\n");
    printf("  nord list A [-d] [-v]             List all 16 slots in bank A\n");
    printf("  nord status                        Connection info\n");
    printf("  nord copy SRC DST [-p N]           Copy program\n");
    printf("  nord move SRC DST [-p N]           Move program\n");
    printf("  nord swap A B [-p N]               Swap two programs\n");
    printf("  nord rename ADDR \"Name\" [-p N]     Rename program\n");
    printf("  nord delete ADDR [-p N]            Delete program\n");
    printf("  nord export ADDR FILE [-p N]       Download program to file\n");
    printf("  nord import FILE ADDR [-p N]       Upload file to program slot\n");
    printf("  nord plan --before F --after F [--json]\n");
    printf("                                     Compute optimal move/swap sequence\n");
    printf("                                     between two slot snapshots (offline)\n");
    printf("\n");
    printf("Address format: BANK:PAGE:PROG (e.g. A:1:3, F:2:1)\n");
    printf("  Matches `nord list` output and the physical keyboard layout.\n");
    printf("  BANK = A-Z, PAGE = 1-4, PROG = 1-4\n");
    printf("  Just 'A' defaults to A:1:1.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -p N   Partition (default: 4=Program, 5=Live)\n");
    printf("  -d     Show sample dependencies (list only)\n");
    printf("  -v     Verbose: show per-slot sizes and metadata\n");
    printf("  -a     Show empty banks too (list only)\n");
    printf("  -h     This help\n");
}

// ─── Main ───

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 0; }

    const char *cmd = argv[1];

    if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "help") == 0) {
        usage();
        return 0;
    }

    if (strcmp(cmd, "--version") == 0 || strcmp(cmd, "-V") == 0) {
        printf("nord 0.2.0\n");
        return 0;
    }

    // Parse global options from remaining args
    int partId = 4;  // default: Program partition
    bool showDeps = false;
    bool showEmpty = false;
    bool verbose = false;
    bool listAll = false;
    bool jsonOut = false;
    const char *beforePath = NULL;
    const char *afterPath = NULL;

    // Collect non-flag args
    const char *posArgs[16];
    int nPos = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            partId = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-d") == 0) {
            showDeps = true;
        } else if (strcmp(argv[i], "-a") == 0) {
            showEmpty = true;
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--all") == 0) {
            listAll = true;
        } else if (strcmp(argv[i], "--json") == 0) {
            jsonOut = true;
        } else if (strcmp(argv[i], "--before") == 0 && i + 1 < argc) {
            beforePath = argv[++i];
        } else if (strcmp(argv[i], "--after") == 0 && i + 1 < argc) {
            afterPath = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0) {
            usage();
            return 0;
        } else {
            if (nPos < 16) posArgs[nPos++] = argv[i];
        }
    }

    // `plan` is offline — no hardware required.
    if (strcmp(cmd, "plan") == 0) {
        return cmd_plan(beforePath, afterPath, jsonOut);
    }

    // Connect
    NordDevice *nd = connect_nord();
    if (!nd) return 1;

    int ret = 0;

    if (strcmp(cmd, "status") == 0) {
        ret = cmd_status(nd);
    } else if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0) {
        // Check if first positional arg is a bank letter
        int filterBank = -1;
        if (nPos >= 1 && strlen(posArgs[0]) == 1) {
            char c = toupper(posArgs[0][0]);
            if (c >= 'A' && c <= 'Z') filterBank = c - 'A';
        }
        ret = cmd_list(nd, partId, showDeps, showEmpty, verbose, listAll, filterBank);
    } else if (strcmp(cmd, "copy") == 0 || strcmp(cmd, "cp") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord copy SRC DST\n");
            ret = 1;
        } else {
            BankSpec src, dst;
            if (!parse_bank_spec(posArgs[0], &src) || !parse_bank_spec(posArgs[1], &dst))
                ret = 1;
            else
                ret = cmd_copy(nd, partId, &src, &dst);
        }
    } else if (strcmp(cmd, "move") == 0 || strcmp(cmd, "mv") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord move SRC DST\n");
            ret = 1;
        } else {
            BankSpec src, dst;
            if (!parse_bank_spec(posArgs[0], &src) || !parse_bank_spec(posArgs[1], &dst))
                ret = 1;
            else
                ret = cmd_move(nd, partId, &src, &dst);
        }
    } else if (strcmp(cmd, "swap") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord swap BANK1 BANK2\n");
            ret = 1;
        } else {
            BankSpec a, b;
            if (!parse_bank_spec(posArgs[0], &a) || !parse_bank_spec(posArgs[1], &b))
                ret = 1;
            else
                ret = cmd_swap(nd, partId, &a, &b);
        }
    } else if (strcmp(cmd, "rename") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord rename BANK \"Name\"\n");
            ret = 1;
        } else {
            BankSpec spec;
            if (!parse_bank_spec(posArgs[0], &spec))
                ret = 1;
            else
                ret = cmd_rename(nd, partId, &spec, posArgs[1]);
        }
    } else if (strcmp(cmd, "delete") == 0 || strcmp(cmd, "rm") == 0) {
        if (nPos < 1) {
            fprintf(stderr, "Usage: nord delete BANK\n");
            ret = 1;
        } else {
            BankSpec spec;
            if (!parse_bank_spec(posArgs[0], &spec))
                ret = 1;
            else
                ret = cmd_delete(nd, partId, &spec);
        }
    } else if (strcmp(cmd, "export") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord export ADDR FILE\n");
            ret = 1;
        } else {
            BankSpec spec;
            if (!parse_bank_spec(posArgs[0], &spec))
                ret = 1;
            else
                ret = cmd_export(nd, partId, &spec, posArgs[1]);
        }
    } else if (strcmp(cmd, "import") == 0) {
        if (nPos < 2) {
            fprintf(stderr, "Usage: nord import FILE ADDR\n");
            ret = 1;
        } else {
            BankSpec spec;
            if (!parse_bank_spec(posArgs[1], &spec))
                ret = 1;
            else
                ret = cmd_import(nd, partId, posArgs[0], &spec);
        }
    } else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        usage();
        ret = 1;
    }

    disconnect_nord(nd);
    return ret;
}
