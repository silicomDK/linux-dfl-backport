.. SPDX-License-Identifier: GPL-2.0

=========================
FPGA Image Load Framework
=========================

The FPGA Image Load framework provides a common API for user-space
tools to manage image uploads to FPGA devices. Device drivers that
instantiate the FPGA Image Load framework will interact with the
target device to transfer and authenticate the image data. Image uploads
are processed in the context of a kernel worker thread.

User API
========

open
----

An fpga_image_load device is opened exclusively to control an image upload.
The device must remain open throughout the duration of the image upload.
An attempt to close the device while an upload is in progress will cause
the upload to be cancelled. If unable to cancel the image upload, the close
system call will block until the image upload is complete.

ioctl
-----

FPGA_IMAGE_LOAD_WRITE:

Start an image upload with the provided image buffer. This IOCTL returns
immediately after starting a kernel worker thread to process the image
upload which could take as long as 40 minutes depending on the actual device
being updated. This is an exclusive operation; an attempt to start
concurrent image uploads for the same device will fail with EBUSY. An
eventfd file descriptor parameter is provided to this IOCTL. It will be
signalled at the completion of the image upload.

FPGA_IMAGE_LOAD_STATUS:

Collect status for an on-going image upload. The status returned includes
how much data remains to be transferred, the progress of the image upload,
and error information in the case of a failure.

FPGA_IMAGE_LOAD_CANCEL:

Request that an on-going image upload be cancelled. This IOCTL will return
ENODEV if there is no update in progress. Depending on the implementation
of the lower-level driver, the cancellation may take affect immediately.
In other cases, the cancellation request may be blocked until a critical
operation such as a FLASH is safely completed.
