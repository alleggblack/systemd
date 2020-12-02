/*
 * System Memory information
 *
 *   Copyright (C) 2000-2002 Alan Cox <alan@redhat.com>
 *   Copyright (C) 2002-2020 Jean Delvare <jdelvare@suse.de>
 *   Copyright (C) 2020 Bastien Nocera <hadess@hadess.net>
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 *   For the avoidance of doubt the "preferred form" of this code is one which
 *   is in an open unpatent encumbered format. Where cryptographic key signing
 *   forms part of the process of creating an executable the information
 *   including keys needed to generate an equivalently functional executable
 *   are deemed to be part of the source code.
 *
 * Unless specified otherwise, all references are aimed at the "System
 * Management BIOS Reference Specification, Version 3.2.0" document,
 * available from http://www.dmtf.org/standards/smbios.
 *
 * Note to contributors:
 * Please reference every value you add or modify, especially if the
 * information does not come from the above mentioned specification.
 *
 * Additional references:
 *  - Intel AP-485 revision 36
 *    "Intel Processor Identification and the CPUID Instruction"
 *    http://www.intel.com/support/processors/sb/cs-009861.htm
 *  - DMTF Common Information Model
 *    CIM Schema version 2.19.1
 *    http://www.dmtf.org/standards/cim/
 *  - IPMI 2.0 revision 1.0
 *    "Intelligent Platform Management Interface Specification"
 *    http://developer.intel.com/design/servers/ipmi/spec.htm
 *  - AMD publication #25481 revision 2.28
 *    "CPUID Specification"
 *    http://www.amd.com/us-en/assets/content_type/white_papers_and_tech_docs/25481.pdf
 *  - BIOS Integrity Services Application Programming Interface version 1.0
 *    http://www.intel.com/design/archives/wfm/downloads/bisspec.htm
 *  - DMTF DSP0239 version 1.1.0
 *    "Management Component Transport Protocol (MCTP) IDs and Codes"
 *    http://www.dmtf.org/standards/pmci
 *  - "TPM Main, Part 2 TPM Structures"
 *    Specification version 1.2, level 2, revision 116
 *    https://trustedcomputinggroup.org/tpm-main-specification/
 *  - "PC Client Platform TPM Profile (PTP) Specification"
 *    Family "2.0", Level 00, Revision 00.43, January 26, 2015
 *    https://trustedcomputinggroup.org/pc-client-platform-tpm-profile-ptp-specification/
 *  - "RedFish Host Interface Specification" (DMTF DSP0270)
 *    https://www.dmtf.org/sites/default/files/DSP0270_1.0.1.pdf
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <getopt.h>
#define _GNU_SOURCE
#include <errno.h>

#include "build.h"
#include "fileio.h"
#include "udev-util.h"
#include "types.h"

#define out_of_spec "<OUT OF SPEC>"
static const char *bad_index = "<BAD INDEX>";

#define SUPPORTED_SMBIOS_VER 0x030300

#define FLAG_NO_FILE_OFFSET     (1 << 0)
#define FLAG_STOP_AT_EOT        (1 << 1)
#define FLAG_FROM_DUMP          (1 << 2)
static uint32_t opt_flags = 0;

#define SYS_FIRMWARE_DIR "/sys/firmware/dmi/tables"
#define SYS_ENTRY_FILE SYS_FIRMWARE_DIR "/smbios_entry_point"
#define SYS_TABLE_FILE SYS_FIRMWARE_DIR "/DMI"

static int checksum(const uint8_t *buf, size_t len) {
        uint8_t sum = 0;
        size_t a;

        for (a = 0; a < len; a++)
                sum += buf[a];
        return (sum == 0);
}

/*
 * Type-independant Stuff
 */

static char *_dmi_string(const struct dmi_header *dm, uint8_t s)
{
        char *bp = (char *)dm->data;

        bp += dm->length;
        while (s > 1 && *bp)
        {
                bp += strlen(bp);
                bp++;
                s--;
        }

        if (!*bp)
                return NULL;

        return bp;
}

static const char *dmi_string(const struct dmi_header *dm, uint8_t s)
{
        char *bp;

        if (s == 0)
                return "Not Specified";

        bp = _dmi_string(dm, s);
        if (bp == NULL)
                return bad_index;

        return bp;
}

/* shift is 0 if the value is in bytes, 1 if it is in kilobytes */
static void dmi_print_memory_size(const char *attr, int slot_num, u64 code, int shift)
{
        uint64_t capacity;

        capacity = code.h;
        capacity = capacity << 32 | code.l;
        if (shift == 1)
                capacity = capacity << 10;

        if (slot_num >= 0)
                printf("%s%u=%lu\n", attr, slot_num, capacity);
        else
                printf("%s=%lu\n", attr, capacity);
}

/*
 * 7.17 Physical Memory Array (Type 16)
 */

static const char *dmi_memory_array_location(uint8_t code)
{
        /* 7.17.1 */
        static const char *location[] = {
                "Other", /* 0x01 */
                "Unknown",
                "System Board Or Motherboard",
                "ISA Add-on Card",
                "EISA Add-on Card",
                "PCI Add-on Card",
                "MCA Add-on Card",
                "PCMCIA Add-on Card",
                "Proprietary Add-on Card",
                "NuBus" /* 0x0A */
        };
        static const char *location_0xA0[] = {
                "PC-98/C20 Add-on Card", /* 0xA0 */
                "PC-98/C24 Add-on Card",
                "PC-98/E Add-on Card",
                "PC-98/Local Bus Add-on Card",
                "CXL Flexbus 1.0" /* 0xA4 */
        };

        if (code >= 0x01 && code <= 0x0A)
                return location[code - 0x01];
        if (code >= 0xA0 && code <= 0xA4)
                return location_0xA0[code - 0xA0];
        return out_of_spec;
}

static const char *dmi_memory_array_ec_type(uint8_t code)
{
        /* 7.17.3 */
        static const char *type[] = {
                "Other", /* 0x01 */
                "Unknown",
                "None",
                "Parity",
                "Single-bit ECC",
                "Multi-bit ECC",
                "CRC" /* 0x07 */
        };

        if (code >= 0x01 && code <= 0x07)
                return type[code - 0x01];
        return out_of_spec;
}

/*
 * 7.18 Memory Device (Type 17)
 */

static void dmi_memory_device_width(const char *attr, unsigned int slot_num, uint16_t code)
{
        /*
         * If no memory module is present, width may be 0
         */
        if (code != 0xFFFF && code != 0)
                printf("%s%u=%u\n", attr, slot_num, code);
}

static void dmi_memory_device_size(unsigned int slot_num, uint16_t code)
{
        if (code == 0) {
                printf("MEMORY_DEVICE_PRESENT%u=0\n", slot_num);
                return;
        } else if (code == 0xFFFF) {
                return;
        } else {
                u64 s = { .l = code & 0x7FFF };
                if (!(code & 0x8000))
                        s.l <<= 10;
                dmi_print_memory_size("MEMORY_DEVICE_SIZE", slot_num, s, 1);
        }
}

static void dmi_memory_device_extended_size(unsigned int slot_num, uint32_t code)
{
        uint64_t capacity;
        code &= 0x7FFFFFFFUL;

        /*
         * Use the greatest unit for which the exact value can be displayed
         * as an integer without rounding
         */
        if (code & 0x3FFUL)
                capacity = code * 1024^2;
        else if (code & 0xFFC00UL)
                capacity = code * 1024^3;
        else
                capacity = code * 1024^4;

        printf("MEMORY_DEVICE_SIZE%u=%lu\n", slot_num, capacity);
}

static void dmi_memory_voltage_value(const char *attr, unsigned int slot_num, uint16_t code)
{
        if (code == 0)
                return;
        if (code % 100)
                printf("%s%u=%g\n", attr, slot_num, (float)code / 1000);
        else
                printf("%s%u=%.1f\n", attr, slot_num, (float)code / 1000);
}

static const char *dmi_memory_device_form_factor(uint8_t code)
{
        /* 7.18.1 */
        static const char *form_factor[] = {
                "Other", /* 0x01 */
                "Unknown",
                "SIMM",
                "SIP",
                "Chip",
                "DIP",
                "ZIP",
                "Proprietary Card",
                "DIMM",
                "TSOP",
                "Row Of Chips",
                "RIMM",
                "SODIMM",
                "SRIMM",
                "FB-DIMM",
                "Die" /* 0x10 */
        };

        if (code >= 0x01 && code <= 0x10)
                return form_factor[code - 0x01];
        return out_of_spec;
}

static void dmi_memory_device_set(unsigned int slot_num, uint8_t code)
{
        if (code == 0xFF)
                printf("MEMORY_DEVICE_SET%u=%s\n", slot_num, "Unknown");
        else if (code != 0)
                printf("MEMORY_DEVICE_SET%u=%u\n", slot_num, code);
}

static const char *dmi_memory_device_type(uint8_t code)
{
        /* 7.18.2 */
        static const char *type[] = {
                "Other", /* 0x01 */
                "Unknown",
                "DRAM",
                "EDRAM",
                "VRAM",
                "SRAM",
                "RAM",
                "ROM",
                "Flash",
                "EEPROM",
                "FEPROM",
                "EPROM",
                "CDRAM",
                "3DRAM",
                "SDRAM",
                "SGRAM",
                "RDRAM",
                "DDR",
                "DDR2",
                "DDR2 FB-DIMM",
                "Reserved",
                "Reserved",
                "Reserved",
                "DDR3",
                "FBD2",
                "DDR4",
                "LPDDR",
                "LPDDR2",
                "LPDDR3",
                "LPDDR4",
                "Logical non-volatile device",
                "HBM",
                "HBM2" /* 0x21 */
        };

        if (code >= 0x01 && code <= 0x21)
                return type[code - 0x01];
        return out_of_spec;
}

static void dmi_memory_device_type_detail(unsigned int slot_num, uint16_t code)
{
        /* 7.18.3 */
        static const char *detail[] = {
                "Other", /* 1 */
                "Unknown",
                "Fast-paged",
                "Static Column",
                "Pseudo-static",
                "RAMBus",
                "Synchronous",
                "CMOS",
                "EDO",
                "Window DRAM",
                "Cache DRAM",
                "Non-Volatile",
                "Registered (Buffered)",
                "Unbuffered (Unregistered)",
                "LRDIMM"  /* 15 */
        };
        char list[172];		/* Update length if you touch the array above */

        if ((code & 0xFFFE) == 0)
                printf("MEMORY_DEVICE_TYPE_DETAIL%u=%s\n", slot_num, "None");
        else
        {
                int i, off = 0;

                list[0] = '\0';
                for (i = 1; i <= 15; i++)
                        if (code & (1 << i))
                                off += sprintf(list + off, off ? " %s" : "%s",
                                               detail[i - 1]);
                printf("MEMORY_DEVICE_TYPE_DETAIL%u=%s\n", slot_num, list);
        }
}

static void dmi_memory_device_speed(const char *attr, unsigned int slot_num, uint16_t code)
{
        if (code != 0)
                printf("%s%u=%u\n", attr, slot_num, code);
}

static void dmi_memory_technology(unsigned int slot_num, uint8_t code)
{
        /* 7.18.6 */
        static const char * const technology[] = {
                "Other", /* 0x01 */
                "Unknown",
                "DRAM",
                "NVDIMM-N",
                "NVDIMM-F",
                "NVDIMM-P",
                "Intel Optane DC persistent memory" /* 0x07 */
        };
        if (code >= 0x01 && code <= 0x07)
                printf("MEMORY_DEVICE_MEMORY_TECHNOLOGY%u=%s\n", slot_num, technology[code - 0x01]);
        else
                printf("MEMORY_DEVICE_MEMORY_TECHNOLOGY%u=%s\n", slot_num, out_of_spec);
}

static void dmi_memory_operating_mode_capability(unsigned int slot_num, uint16_t code)
{
        /* 7.18.7 */
        static const char * const mode[] = {
                "Other", /* 1 */
                "Unknown",
                "Volatile memory",
                "Byte-accessible persistent memory",
                "Block-accessible persistent memory" /* 5 */
        };
        char list[99];		/* Update length if you touch the array above */

        if ((code & 0xFFFE) != 0) {
                int i, off = 0;

                list[0] = '\0';
                for (i = 1; i <= 5; i++)
                        if (code & (1 << i))
                                off += sprintf(list + off, off ? " %s" : "%s",
                                               mode[i - 1]);
                printf("MEMORY_DEVICE_MEMORY_OPERATING_MODE_CAPABILITY%u=%s\n", slot_num, list);
        }
}

static void dmi_memory_manufacturer_id(const char *attr, unsigned int slot_num, uint16_t code)
{
        /* 7.18.8 */
        /* 7.18.10 */
        /* LSB is 7-bit Odd Parity number of continuation codes */
        if (code != 0)
                printf("%s%u=Bank %d, Hex 0x%02X\n", attr, slot_num, (code & 0x7F) + 1, code >> 8);
}

static void dmi_memory_product_id(const char *attr, unsigned int slot_num, uint16_t code)
{
        /* 7.18.9 */
        /* 7.18.11 */
        if (code != 0)
                printf("%s%u=0x%04X\n", attr, slot_num, code);
}

static void dmi_memory_size(const char *attr, unsigned int slot_num, u64 code)
{
        /* 7.18.12 */
        /* 7.18.13 */
        if ((code.h == 0xFFFFFFFF && code.l == 0xFFFFFFFF) ||
            (code.h == 0x0 && code.l == 0x0))
                return;
        dmi_print_memory_size(attr, slot_num, code, 0);
}

/*
 * Main
 */

static void dmi_decode(const struct dmi_header *h)
{
        const uint8_t *data = h->data;
        static unsigned int next_slot_num = 0;
        unsigned int slot_num;

        /*
         * Note: DMI types 37 and 42 are untested
         */
        switch (h->type)
        {
        case 16: /* 7.17 Physical Memory Array */
                log_debug("Physical Memory Array");
                if (h->length < 0x0F) break;
                if (data[0x05] != 0x03) break; /* 7.17.2, Use == "System Memory" */
                log_debug("Use: System Memory");
                printf("MEMORY_ARRAY_LOCATION=%s\n",
                       dmi_memory_array_location(data[0x04]));
                if (data[0x06] != 0x03) {
                        printf("MEMORY_ARRAY_EC_TYPE=%s\n",
                               dmi_memory_array_ec_type(data[0x06]));
                }
                if (DWORD(data + 0x07) == 0x80000000) {
                        if (h->length >= 0x17)
                                dmi_print_memory_size("MEMORY_ARRAY_MAX_CAPACITY", -1, QWORD(data + 0x0F), 0);
                } else {
                        u64 capacity;

                        capacity.h = 0;
                        capacity.l = DWORD(data + 0x07);
                        dmi_print_memory_size("MEMORY_ARRAY_MAX_CAPACITY", -1, capacity, 1);
                }
                printf("MEMORY_ARRAY_NUM_DEVICES=%u\n", WORD(data + 0x0D));
                break;

        case 17: /* 7.18 Memory Device */
                slot_num = next_slot_num;
                next_slot_num++;

                log_debug("Memory Device");
                if (h->length < 0x15) break;
                dmi_memory_device_width("MEMORY_DEVICE_TOTAL_WIDTH", slot_num, WORD(data + 0x08));
                dmi_memory_device_width("MEMORY_DEVICE_DATA_WIDTH", slot_num, WORD(data + 0x0A));
                if (h->length >= 0x20 && WORD(data + 0x0C) == 0x7FFF)
                        dmi_memory_device_extended_size(slot_num, DWORD(data + 0x1C));
                else
                        dmi_memory_device_size(slot_num, WORD(data + 0x0C));
                if (data[0x0E] != 0x02) {
                        printf("MEMORY_DEVICE_FORM_FACTOR%u=%s\n", slot_num,
                               dmi_memory_device_form_factor(data[0x0E]));
                }
                dmi_memory_device_set(slot_num, data[0x0F]);
                printf("MEMORY_DEVICE_LOCATOR%u=%s\n", slot_num, dmi_string(h, data[0x10]));
                printf("MEMORY_DEVICE_BANK_LOCATOR%u=%s\n", slot_num, dmi_string(h, data[0x11]));
                printf("MEMORY_DEVICE_TYPE%u=%s\n", slot_num, dmi_memory_device_type(data[0x12]));
                dmi_memory_device_type_detail(slot_num, WORD(data + 0x13));
                if (h->length < 0x17) break;
                dmi_memory_device_speed("MEMORY_DEVICE_SPEED_MTS", slot_num, WORD(data + 0x15));
                if (h->length < 0x1B) break;
                printf("MEMORY_DEVICE_MANUFACTURER%u=%s\n", slot_num, dmi_string(h, data[0x17]));
                printf("MEMORY_DEVICE_SERIAL_NUMBER%u=%s\n", slot_num, dmi_string(h, data[0x18]));
                printf("MEMORY_DEVICE_ASSET_TAG%u=%s\n", slot_num, dmi_string(h, data[0x19]));
                printf("MEMORY_DEVICE_PART_NUMBER%u=%s\n", slot_num, dmi_string(h, data[0x1A]));
                if (h->length < 0x1C) break;
                if ((data[0x1B] & 0x0F) != 0)
                        printf("MEMORY_DEVICE_RANK%u=%u\n", slot_num, data[0x1B] & 0x0F);
                if (h->length < 0x22) break;
                dmi_memory_device_speed("MEMORY_DEVICE_CONFIGURED_SPEED_MTS", slot_num, WORD(data + 0x20));
                if (h->length < 0x28) break;
                dmi_memory_voltage_value("MEMORY_DEVICE_MINIMUM_VOLTAGE", slot_num, WORD(data + 0x22));
                dmi_memory_voltage_value("MEMORY_DEVICE_MAXIMUM_VOLTAGE", slot_num, WORD(data + 0x24));
                dmi_memory_voltage_value("MEMORY_DEVICE_CONFIGURED_VOLTAGE", slot_num, WORD(data + 0x26));
                if (h->length < 0x34) break;
                dmi_memory_technology(slot_num, data[0x28]);
                dmi_memory_operating_mode_capability(slot_num, WORD(data + 0x29));
                printf("MEMORY_DEVICE_FIRMWARE_VERSION%u=%s\n", slot_num, dmi_string(h, data[0x2B]));
                dmi_memory_manufacturer_id("MEMORY_DEVICE_MODULE_MANUFACTURER_ID", slot_num, WORD(data + 0x2C));
                dmi_memory_product_id("MEMORY_DEVICE_MODULE_PRODUCT_ID", slot_num, WORD(data + 0x2E));
                dmi_memory_manufacturer_id("MEMORY_DEVICE_MEMORY_SUBSYSTEM_CONTROLLER_MANUFACTURER_ID",
                                           slot_num, WORD(data + 0x30));
                dmi_memory_product_id("MEMORY_DEVICE_MEMORY_SUBSYSTEM_CONTROLLER_PRODUCT_ID",
                                      slot_num, WORD(data + 0x32));
                if (h->length < 0x3C) break;
                dmi_memory_size("MEMORY_DEVICE_NON_VOLATILE_SIZE", slot_num, QWORD(data + 0x34));
                if (h->length < 0x44) break;
                dmi_memory_size("MEMORY_DEVICE_VOLATILE_SIZE", slot_num, QWORD(data + 0x3C));
                if (h->length < 0x4C) break;
                dmi_memory_size("MEMORY_DEVICE_CACHE_SIZE", slot_num, QWORD(data + 0x44));
                if (h->length < 0x54) break;
                dmi_memory_size("MEMORY_DEVICE_LOGICAL_SIZE", slot_num, QWORD(data + 0x4C));
                break;
        }
}

static void to_dmi_header(struct dmi_header *h, uint8_t *data)
{
        h->type = data[0];
        h->length = data[1];
        h->handle = WORD(data + 2);
        h->data = data;
}

static void dmi_table_decode(uint8_t *buf, uint32_t len, uint16_t num, uint32_t flags)
{
        uint8_t *data;
        int i = 0;

        data = buf;
        while ((i < num || !num)
               && data + 4 <= buf + len) /* 4 is the length of an SMBIOS structure header */
        {
                uint8_t *next;
                struct dmi_header h;
                int display;

                to_dmi_header(&h, data);
                display = (!(h.type == 126 || h.type == 127));

                /*
                 * If a short entry is found (less than 4 bytes), not only it
                 * is invalid, but we cannot reliably locate the next entry.
                 * Better stop at this point, and let the user know his/her
                 * table is broken.
                 */
                if (h.length < 4)
                        break;
                i++;

                /* In quiet mode, stop decoding at end of table marker */
                if (h.type == 127)
                        break;

                /* Look for the next handle */
                next = data + h.length;
                while ((unsigned long)(next - buf + 1) < len
                       && (next[0] != 0 || next[1] != 0))
                        next++;
                next += 2;

                /* Make sure the whole structure fits in the table */
                if ((unsigned long)(next - buf) > len)
                {
                        data = next;
                        break;
                }

                if (display)
                        dmi_decode(&h);

                data = next;

                /* SMBIOS v3 requires stopping at this marker */
                if (h.type == 127 && (flags & FLAG_STOP_AT_EOT))
                        break;
        }
}

static void dmi_table(off_t base, uint32_t len, uint16_t num, const char *devmem,
                      uint32_t flags)
{
        uint8_t *buf;
        size_t size;

        /*
         * When reading from sysfs or from a dump file, the file may be
         * shorter than announced. For SMBIOS v3 this is expcted, as we
         * only know the maximum table size, not the actual table size.
         * For older implementations (and for SMBIOS v3 too), this
         * would be the result of the kernel truncating the table on
         * parse error.
         */
        if (read_full_file_full(AT_FDCWD, devmem, flags & FLAG_NO_FILE_OFFSET ? 0 : base, len, 0, NULL, (char **) &buf, &size) < 0) {
                log_error("Failed to read table, sorry.");
                return;
        }

        len = size;
        dmi_table_decode(buf, len, num, flags);

        free(buf);
}


/* Same thing for SMBIOS3 entry points */
static int smbios3_decode(uint8_t *buf, const char *devmem, uint32_t flags)
{
        u64 offset;

        /* Don't let checksum run beyond the buffer */
        if (buf[0x06] > 0x20)
        {
                log_error("Entry point length too large (%u bytes, expected %u).",
                          (unsigned int)buf[0x06], 0x18U);
                return 0;
        }

        if (!checksum(buf, buf[0x06]))
                return 0;

        offset = QWORD(buf + 0x10);
        if (!(flags & FLAG_NO_FILE_OFFSET) && offset.h && sizeof(off_t) < 8)
        {
                log_error("64-bit addresses not supported, sorry.");
                return 0;
        }

        dmi_table(((off_t)offset.h << 32) | offset.l,
                  DWORD(buf + 0x0C), 0, devmem, flags | FLAG_STOP_AT_EOT);

        return 1;
}

static int smbios_decode(uint8_t *buf, const char *devmem, uint32_t flags)
{
        /* Don't let checksum run beyond the buffer */
        if (buf[0x05] > 0x20) {
                log_error("Entry point length too large (%u bytes, expected %u).",
                          (unsigned int)buf[0x05], 0x1FU);
                return 0;
        }

        if (!checksum(buf, buf[0x05])
            || memcmp(buf + 0x10, "_DMI_", 5) != 0
            || !checksum(buf + 0x10, 0x0F))
                return 0;

        dmi_table(DWORD(buf + 0x18), WORD(buf + 0x16), WORD(buf + 0x1C),
                  devmem, flags);

        return 1;
}

static int legacy_decode(uint8_t *buf, const char *devmem, uint32_t flags)
{
        if (!checksum(buf, 0x0F))
                return 0;

        dmi_table(DWORD(buf + 0x08), WORD(buf + 0x06), WORD(buf + 0x0C),
                  devmem, flags);

        return 1;
}

int main(int argc, char * const argv[])
{
        static const struct option options[] = {
                { "help", no_argument, NULL, 'h' },
                { "from-dump", required_argument, NULL, 'F' },
                { "version", no_argument, NULL, 'V' },
                {}
        };
        const char *dump_file = NULL;

        int ret = 0;                /* Returned value */
        int found = 0;
        size_t size;
        uint8_t *buf = NULL;
        uint32_t flags = 0;

        log_set_target(LOG_TARGET_AUTO);
        udev_parse_config();
        log_parse_environment();
        log_open();

        for (;;) {
                int option;

                option = getopt_long(argc, argv, "F:hV", options, NULL);
                if (option == -1)
                        break;

                switch (option) {
                case 'F':
                        opt_flags |= FLAG_FROM_DUMP;
                        dump_file = optarg;
                        break;
                case 'h':
                        printf("Usage: %s [options]\n"
                               " -F,--from-dump FILE   read DMI information from a binary file\n"
                               " -h,--help             print this help text\n\n",
                               program_invocation_short_name);
                        exit(EXIT_SUCCESS);
                case 'V':
                        printf("%s\n", GIT_VERSION);
                        exit(EXIT_SUCCESS);
                case '?':
                        return -1;
                }
        }

        /* Read from dump if so instructed */
        if (read_full_file_full(AT_FDCWD,
                                opt_flags & FLAG_FROM_DUMP ? dump_file : SYS_ENTRY_FILE,
                                0, 0x20, 0, NULL, (char **) &buf, &size) < 0) {
                log_debug_errno(errno, "reading DMI from %s failed",
                                opt_flags & FLAG_FROM_DUMP ? dump_file : SYS_ENTRY_FILE);
                ret = EXIT_FAILURE;
                goto exit_free;
        }
        if (!(opt_flags & FLAG_FROM_DUMP)) {
                dump_file = SYS_TABLE_FILE;
                flags = FLAG_NO_FILE_OFFSET;
        }

        if (size >= 24 && memcmp(buf, "_SM3_", 5) == 0) {
                if (smbios3_decode(buf, dump_file, flags))
                        found++;
        } else if (size >= 31 && memcmp(buf, "_SM_", 4) == 0) {
                if (smbios_decode(buf, dump_file, flags))
                        found++;
        } else if (size >= 15 && memcmp(buf, "_DMI_", 5) == 0) {
                if (legacy_decode(buf, dump_file, flags))
                        found++;
        }

        if (!found)
                ret = EXIT_FAILURE;

        free(buf);
exit_free:

        return ret;
}
