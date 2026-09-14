/****************************************************************************
 * apps/boot/rp2040boot/rp2040boot_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <elf.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/boardctl.h>

#include <nuttx/lib/elf.h>

#ifdef CONFIG_CDCACM
#include <nuttx/usb/cdcacm.h>
#include <nuttx/usb/cdc.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* RP2040 SRAM end address */

#define RP2040_SRAM_END  0x20042000

/****************************************************************************
 * External Functions
 ****************************************************************************/

/* Board-side jump: disables interrupts/systick, sets MSP, jumps.
 * Defined in boards/arm/rp2040/common/src/rp2040_boot_elf.c
 */

extern void rp2040_boot_jump(uint32_t msp, uint32_t reset);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: verify_phdr
 *
 * Description:
 *   Validate a single PT_LOAD program header before calling libelf_load().
 *   libelf internally does not perform these range checks.
 *
 * NOTE: This validation covers PT_LOAD segments only.  Sections with
 * SHF_ALLOC that fall outside any phdr range bypass these checks.
 * This is a sanity net, not a security boundary.  Only trusted ELF
 * images should be loaded.
 *
 ****************************************************************************/

static int verify_phdr(FAR Elf_Phdr *phdr)
{
  uintptr_t load_end;

  if (phdr->p_type != PT_LOAD)
    {
      return OK;
    }

  /* p_filesz must be <= p_memsz (check before memsz==0 skip) */

  if (phdr->p_filesz > phdr->p_memsz)
    {
      syslog(LOG_ERR, "p_filesz(0x%" PRIx32 ") > p_memsz(0x%" PRIx32 ")\n",
             (uint32_t)phdr->p_filesz, (uint32_t)phdr->p_memsz);
      return -EINVAL;
    }

  /* Skip segments with zero memory size (e.g. .flash_section placeholder) */

  if (phdr->p_memsz == 0)
    {
      return OK;
    }

  /* Check integer overflow on offset+filesz, vaddr+memsz, paddr+memsz */

  if (phdr->p_offset + phdr->p_filesz < phdr->p_offset)
    {
      syslog(LOG_ERR, "Integer overflow: p_offset=0x%" PRIx32
             " p_filesz=0x%" PRIx32 "\n",
             (uint32_t)phdr->p_offset, (uint32_t)phdr->p_filesz);
      return -EINVAL;
    }

  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    {
      syslog(LOG_ERR, "Integer overflow: p_vaddr=0x%" PRIx32
             " p_memsz=0x%" PRIx32 "\n",
             (uint32_t)phdr->p_vaddr, (uint32_t)phdr->p_memsz);
      return -EINVAL;
    }

  if (phdr->p_paddr + phdr->p_memsz < phdr->p_paddr)
    {
      syslog(LOG_ERR, "Integer overflow: p_paddr=0x%" PRIx32
             " p_memsz=0x%" PRIx32 "\n",
             (uint32_t)phdr->p_paddr, (uint32_t)phdr->p_memsz);
      return -EINVAL;
    }

  /* Check LMA (p_paddr) is within SRAM and does not overlap bootloader */

  load_end = phdr->p_paddr + phdr->p_memsz;
  if (phdr->p_paddr < CONFIG_BOOT_RP2040BOOT_AP_BASE ||
      load_end > RP2040_SRAM_END)
    {
      syslog(LOG_ERR, "LMA range 0x%" PRIx32 "-0x%" PRIx32
             " outside AP SRAM\n",
             (uint32_t)phdr->p_paddr, (uint32_t)load_end);
      return -EINVAL;
    }

  return OK;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp2040boot_main
 *
 * Description:
 *   RP2040 ELF bootloader entry point.
 *   1. Open and validate the AP ELF header
 *   2. Read program headers, pre-scan PT_LOAD segments for safety
 *   3. Load segments via libelf (LOADTO_LMA, skip bind)
 *   4. Read vector table from loaded memory
 *   5. Jump via board-side rp2040_boot_jump()
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct mod_loadinfo_s loadinfo;
  FAR Elf_Phdr *phdr = NULL;
  size_t phdrsize;
  uintptr_t vt_addr;
  uint32_t msp;
  uint32_t reset;
  int ret;
  int i;

#ifdef CONFIG_BOARDCTL_FINALINIT
  boardctl(BOARDIOC_FINALINIT, 0);
#endif

  /* Wait for CDC console to be ready before producing diagnostic output.
   * On cold boot the host may not have finished USB enumeration yet;
   * without this wait, all syslog output (including ELF load errors)
   * would be lost.
   *
   * Poll CAIOC_GETCTRLLINE until the host opens the port (DTE_PRESENT)
   * or the timeout expires.  This replaces the fixed usleep() with a
   * mechanism that proceeds as soon as the host is ready.
   */

#ifdef CONFIG_CDCACM
  {
    char devpath[16];
    int elapsed;
    int minor = 0;
#ifdef CONFIG_SYSLOG_CDCACM
    minor = CONFIG_SYSLOG_CDCACM_MINOR;
#endif
    snprintf(devpath, sizeof(devpath), "/dev/ttyACM%d", minor);
    for (elapsed = 0;
         elapsed < CONFIG_BOOT_RP2040BOOT_JUMP_DELAY_MS;
         elapsed += 10)
      {
        int fd = open(devpath, O_WRONLY);

        if (fd >= 0)
          {
            int state = 0;

            ioctl(fd, CAIOC_GETCTRLLINE, (unsigned long)&state);
            close(fd);
            if (state & CDC_DTE_PRESENT)
              {
                break;
              }
          }

        usleep(10000);
      }
  }
#elif CONFIG_BOOT_RP2040BOOT_JUMP_DELAY_MS > 0
  usleep(CONFIG_BOOT_RP2040BOOT_JUMP_DELAY_MS * 1000);
#endif

  syslog(LOG_INFO, "*** rp2040boot: ELF bootloader ***\n");

  /* Debug: dump first 16 bytes of AP partition to verify MTD access */

  {
    int dfd = open(CONFIG_BOOT_RP2040BOOT_ELF_PATH, O_RDONLY);

    if (dfd >= 0)
      {
        uint8_t hdr[16];
        ssize_t nr = read(dfd, hdr, sizeof(hdr));

        close(dfd);
        if (nr == sizeof(hdr))
          {
            syslog(LOG_INFO,
                   "AP hdr: %02x %02x %02x %02x %02x %02x %02x %02x "
                   "%02x %02x %02x %02x %02x %02x %02x %02x\n",
                   hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5], hdr[6],
                   hdr[7], hdr[8], hdr[9], hdr[10], hdr[11], hdr[12],
                   hdr[13], hdr[14], hdr[15]);
          }
        else
          {
            syslog(LOG_ERR, "AP hdr read failed: %zd\n", nr);
          }
      }
    else
      {
        syslog(LOG_ERR, "AP open(%s) failed: %d\n",
               CONFIG_BOOT_RP2040BOOT_ELF_PATH, errno);
      }
  }

  /* Step 1: Open ELF and read header */

  ret = libelf_initialize(CONFIG_BOOT_RP2040BOOT_ELF_PATH, &loadinfo);
  if (ret < 0)
    {
      syslog(LOG_ERR, "libelf_initialize failed: %d\n", ret);
      goto errout;
    }

  syslog(LOG_INFO, "ELF opened: filelen=%zu e_phnum=%u\n",
         (size_t)loadinfo.filelen,
         (unsigned)loadinfo.ehdr.e_phnum);

  if (loadinfo.ehdr.e_type != ET_EXEC)
    {
      syslog(LOG_ERR, "Not ET_EXEC (type=%d)\n", loadinfo.ehdr.e_type);
      ret = -ENOEXEC;
      goto errout;
    }

  /* Step 2: Read program headers and pre-validate PT_LOAD segments.
   * We use libelf_read() to read phdr directly from the file rather
   * than calling libelf_loadhdrs(), which is an internal libelf API.
   * libelf_load() will call libelf_loadhdrs() internally later.
   */

  if (loadinfo.ehdr.e_phnum == 0)
    {
      syslog(LOG_ERR, "No program headers\n");
      ret = -ENOEXEC;
      goto errout;
    }

  if (loadinfo.ehdr.e_phentsize != sizeof(Elf_Phdr))
    {
      syslog(LOG_ERR, "e_phentsize %u != sizeof(Elf_Phdr) %zu\n",
             (unsigned)loadinfo.ehdr.e_phentsize, sizeof(Elf_Phdr));
      ret = -ENOEXEC;
      goto errout;
    }

  phdrsize = (size_t)loadinfo.ehdr.e_phentsize *
             (size_t)loadinfo.ehdr.e_phnum;
  if (loadinfo.ehdr.e_phoff + phdrsize < loadinfo.ehdr.e_phoff ||
      loadinfo.ehdr.e_phoff + phdrsize > loadinfo.filelen)
    {
      syslog(LOG_ERR, "Program header table extends past EOF\n");
      ret = -ENOEXEC;
      goto errout;
    }

  phdr = malloc(phdrsize);
  if (phdr == NULL)
    {
      syslog(LOG_ERR, "Failed to allocate phdr (%zu bytes)\n", phdrsize);
      ret = -ENOMEM;
      goto errout;
    }

  ret = libelf_read(&loadinfo, (FAR uint8_t *)phdr, phdrsize,
                    loadinfo.ehdr.e_phoff);
  if (ret < 0)
    {
      syslog(LOG_ERR, "Failed to read program headers: %d\n", ret);
      goto errout;
    }

  for (i = 0; i < loadinfo.ehdr.e_phnum; i++)
    {
      ret = verify_phdr(&phdr[i]);
      if (ret < 0)
        {
          syslog(LOG_ERR, "phdr %d validation failed\n", i);
          goto errout;
        }
    }

  free(phdr);
  phdr = NULL;

  /* Step 3: Load ELF segments.  With LOADTO_LMA, segments land at
   * p_paddr.  We skip libelf_bind() -- ET_EXEC has no relocations.
   * libelf_load() calls libelf_loadhdrs() internally to read both
   * shdr and phdr into loadinfo.
   */

  ret = libelf_load(&loadinfo);
  if (ret < 0)
    {
      syslog(LOG_ERR, "libelf_load failed: %d\n", ret);
      goto errout;
    }

  syslog(LOG_INFO, "libelf_load OK: ret=%d\n", ret);

  /* Step 4: Read vector table from loaded image.
   * With LOADTO_LMA, libelf_load() places segments at p_paddr.
   * The ld script places .vectors at the start of .text, so
   * _vectors is at CONFIG_BOOT_RP2040BOOT_AP_BASE.
   * We do NOT use loadinfo.textalloc which is 0 under LOADTO_LMA.
   */

  vt_addr = CONFIG_BOOT_RP2040BOOT_AP_BASE;

  msp   = *(FAR uint32_t *)vt_addr;
  reset = *(FAR uint32_t *)(vt_addr + 4);

  syslog(LOG_INFO, "MSP=0x%" PRIx32 " Reset=0x%" PRIx32 "\n",
         msp, reset);

  /* Validate MSP and Reset before jumping */

  if (msp < CONFIG_BOOT_RP2040BOOT_AP_BASE || msp > RP2040_SRAM_END)
    {
      syslog(LOG_ERR, "MSP 0x%" PRIx32 " outside AP SRAM\n", msp);
      ret = -EINVAL;
      goto errout;
    }

  if ((reset & ~1u) < CONFIG_BOOT_RP2040BOOT_AP_BASE ||
      (reset & ~1u) >= RP2040_SRAM_END ||
      (reset & 1u) == 0)
    {
      syslog(LOG_ERR, "Reset 0x%" PRIx32 " invalid\n", reset);
      ret = -EINVAL;
      goto errout;
    }

  libelf_uninitialize(&loadinfo);

  /* Drain syslog output before jump.
   * cdcacm_write() returns immediately after queueing bytes; the actual
   * USB IN transfer happens when the host polls.  Without a delay, the
   * USBCTRL reset inside rp2040_boot_jump() kills the transfer mid-flight.
   */

  usleep(100000);

#ifdef CONFIG_BOOT_RP2040BOOT_RESET_USB_BOOT
  /* Debug recovery: reboot into USB BOOTSEL mode instead of jumping.
   * This allows picotool to re-flash without physical BOOTSEL button.
   */

  ret = boardctl(BOARDIOC_RESET, BOARDIOC_SOFTRESETCAUSE_ENTER_BOOTLOADER);
  if (ret < 0)
    {
      syslog(LOG_ERR, "boardctl RESET failed: %d\n", ret);
    }
#endif

  /* Step 5: Jump to AP */

  rp2040_boot_jump(msp, reset);

  /* Should not reach here */

  return 0;

errout:
  free(phdr);
  libelf_uninitialize(&loadinfo);

#ifdef CONFIG_BOOT_RP2040BOOT_RESET_USB_BOOT
  /* On ELF load failure, auto-recover to BOOTSEL so the board
   * re-appears on USB without a physical button press.
   */

  syslog(LOG_ERR, "ELF load failed (ret=%d), rebooting to BOOTSEL\n", ret);
  usleep(100000);
  {
    int bret = boardctl(BOARDIOC_RESET,
                        BOARDIOC_SOFTRESETCAUSE_ENTER_BOOTLOADER);

    if (bret < 0)
      {
        syslog(LOG_ERR, "boardctl RESET failed: %d\n", bret);
      }
  }
#endif

  return ret;
}
