/****************************************************************************
 * apps/boot/rp2040boot_hello/rp2040boot_hello_main.c
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
#include <unistd.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/ioctl.h>

#ifdef CONFIG_CDCACM
#  include <nuttx/usb/cdcacm.h>
#  include <nuttx/usb/cdc.h>
#endif

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: rp2040boot_hello_main
 *
 * Description:
 *   Minimal AP demo that waits for the host to open the CDC/ACM port
 *   (DTE_PRESENT) before printing, so the output is actually captured.
 *
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
#ifdef CONFIG_CDCACM
  char devpath[16];
  int elapsed;
  int minor = 0;

#ifdef CONFIG_SYSLOG_CDCACM
  minor = CONFIG_SYSLOG_CDCACM_MINOR;
#endif

  snprintf(devpath, sizeof(devpath), "/dev/ttyACM%d", minor);

  for (elapsed = 0; elapsed < 5000; elapsed += 10)
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
#endif

  /* Emit a periodic heartbeat rather than a single line so a serial
   * monitor attaching after boot still observes that the AP is running.
   */

  syslog(LOG_INFO, "*** rp2040boot hello: AP is alive ***\n");
  for (int i = 1; ; i++)
    {
      syslog(LOG_INFO, "*** rp2040boot hello: AP heartbeat #%d ***\n", i);
      sleep(1);
    }

  return 0;
}
