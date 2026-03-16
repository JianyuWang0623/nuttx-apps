/****************************************************************************
 * apps/examples/camcap/camcap_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Simple camera capture utility.
 * Captures one RGB565 frame from /dev/video and saves to /tmp/cap.rgb
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#include <nuttx/video/video.h>
#include <nuttx/video/v4l2_cap.h>

#define VIDEO_DEV    "/dev/video"
#define OUTPUT_FILE  "/tmp/cap.rgb"

int main(int argc, FAR char *argv[])
{
  struct v4l2_frmsizeenum frmsize;
  struct v4l2_format fmt;
  struct v4l2_requestbuffers req;
  struct v4l2_buffer buf;
  enum v4l2_buf_type type;
  FAR uint8_t *framebuf;
  uint16_t cap_width;
  uint16_t cap_height;
  uint32_t image_size;
  int v_fd;
  int f_fd;
  int ret;

  printf("camcap: Initializing video...\n");

  ret = capture_initialize(VIDEO_DEV);
  if (ret != 0)
    {
      printf("camcap: capture_initialize failed: %d\n", errno);
      return EXIT_FAILURE;
    }

  v_fd = open(VIDEO_DEV, 0);
  if (v_fd < 0)
    {
      printf("camcap: open %s failed: %d\n", VIDEO_DEV, errno);
      capture_uninitialize(VIDEO_DEV);
      return EXIT_FAILURE;
    }

  /* Query supported resolution from sensor */

  memset(&frmsize, 0, sizeof(frmsize));
  frmsize.index      = 0;
  frmsize.buf_type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  frmsize.pixel_format = V4L2_PIX_FMT_RGB565;

  ret = ioctl(v_fd, VIDIOC_ENUM_FRAMESIZES, (uintptr_t)&frmsize);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_ENUM_FRAMESIZES failed: %d\n", errno);
      goto err;
    }

  cap_width  = frmsize.discrete.width;
  cap_height = frmsize.discrete.height;
  image_size = cap_width * cap_height * 2; /* RGB565 */

  printf("camcap: Sensor resolution: %dx%d\n", cap_width, cap_height);

  /* Set format */

  memset(&fmt, 0, sizeof(fmt));
  fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width       = cap_width;
  fmt.fmt.pix.height      = cap_height;
  fmt.fmt.pix.field       = V4L2_FIELD_ANY;
  fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;

  ret = ioctl(v_fd, VIDIOC_S_FMT, (uintptr_t)&fmt);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_S_FMT failed: %d\n", errno);
      goto err;
    }

  /* Request 1 buffer */

  memset(&req, 0, sizeof(req));
  req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_USERPTR;
  req.count  = 1;
  req.mode   = V4L2_BUF_MODE_FIFO;

  ret = ioctl(v_fd, VIDIOC_REQBUFS, (uintptr_t)&req);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_REQBUFS failed: %d\n", errno);
      goto err;
    }

  /* Allocate frame buffer (32-byte aligned) */

  framebuf = memalign(32, image_size);
  if (!framebuf)
    {
      printf("camcap: Out of memory\n");
      goto err;
    }

  /* Queue buffer */

  memset(&buf, 0, sizeof(buf));
  buf.type      = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory    = V4L2_MEMORY_USERPTR;
  buf.index     = 0;
  buf.m.userptr = (uintptr_t)framebuf;
  buf.length    = image_size;

  ret = ioctl(v_fd, VIDIOC_QBUF, (uintptr_t)&buf);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_QBUF failed: %d\n", errno);
      goto err_free;
    }

  /* Start streaming */

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ret = ioctl(v_fd, VIDIOC_STREAMON, (uintptr_t)&type);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_STREAMON failed: %d\n", errno);
      goto err_free;
    }

  printf("camcap: Capturing frame (%dx%d RGB565)...\n",
         cap_width, cap_height);

  /* Dequeue buffer (blocks until frame is ready) */

  memset(&buf, 0, sizeof(buf));
  buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_USERPTR;

  ret = ioctl(v_fd, VIDIOC_DQBUF, (uintptr_t)&buf);
  if (ret < 0)
    {
      printf("camcap: VIDIOC_DQBUF failed: %d\n", errno);
      goto err_stop;
    }

  printf("camcap: Got frame, %d bytes\n", buf.bytesused);

  /* Driver outputs native BE RGB565 (RGB565X) from sensor.
   * Data is saved as-is; use big-endian decode when converting.
   */

  /* Stop streaming */

  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(v_fd, VIDIOC_STREAMOFF, (uintptr_t)&type);

  /* Save to file */

  f_fd = open(OUTPUT_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (f_fd < 0)
    {
      printf("camcap: open %s failed: %d\n", OUTPUT_FILE, errno);
      goto err_free;
    }

  write(f_fd, (FAR void *)buf.m.userptr, buf.bytesused);
  close(f_fd);

  printf("camcap: Saved to %s (%d bytes)\n", OUTPUT_FILE, buf.bytesused);

  free(framebuf);
  close(v_fd);
  capture_uninitialize(VIDEO_DEV);
  return EXIT_SUCCESS;

err_stop:
  type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  ioctl(v_fd, VIDIOC_STREAMOFF, (uintptr_t)&type);
err_free:
  free(framebuf);
err:
  close(v_fd);
  capture_uninitialize(VIDEO_DEV);
  return EXIT_FAILURE;
}
