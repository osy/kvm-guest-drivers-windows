/*
 * Copyright (C) Turing Software, LLC
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#pragma once

//
// Workaround for the duplicate display bug shared by viogpudo and viogpu3d.
//
// win32k's DpiFdoDetectPostDevice looks for the boot framebuffer address in
// the PDO's resource list. Normally that address comes from the PCI BARs, but
// putting the framebuffer address into the virtio-gpu device's BAR causes
// other issues including host side kernel panics. The least worst solution is
// to hijack the PDO's IRP_MJ_PNP handler and detect when IRP_MN_QUERY_RESOURCES
// is called. We then forward the request, and modify the returned resource
// list to manually add the framebuffer's address into the list.
//
// The hook lives on the PDO's DriverObject (the PCI/ACPI enumerator), which is
// shared by every instance of that enumerator, so Install/Remove maintain a
// single saved dispatch pointer and assume a single virtio-gpu adapter. Call
// VioGpuInstallDisplayFixup() from DxgkDdiAddDevice and
// VioGpuRemoveDisplayFixup() from DxgkDdiRemoveDevice.
//

// Must be Non-Paged (patches a dispatch routine that runs at PASSIVE_LEVEL
// during PnP enumeration, but the pointer must stay resident).
void VioGpuInstallDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject);

void VioGpuRemoveDisplayFixup(_In_ PDEVICE_OBJECT pPhysicalDeviceObject);
