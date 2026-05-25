/*
 * Copyright (C) 2019-2020 Red Hat, Inc.
 *
 * Written By: Vadim Rozenfeld <vrozenfe@redhat.com>
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

#include <d3dkmthk.h>

#pragma pack(1)
typedef struct _VIOGPU_BOX
{
    ULONG x;
    ULONG y;
    ULONG z;
    ULONG width;
    ULONG height;
    ULONG depth;
} VIOGPU_BOX;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_BLOB_INFO {
    ULONG width;
    ULONG height;
    ULONG format;
    ULONG bind; // Same as virgl
    ULONG strides[4];
    ULONG offsets[4];
} VIOGPU_BLOB_INFO, *PVIOGPU_BLOB_INFO;
#pragma pack()

// ================= QueryAdapterInfo UMDRIVERPRIVATE
#define VIOGPU_IAM 0x56696f475055 // Identifier for queryadapterinfo (VioGPU as hex)

#define VIOGPU_CAPSET_GFXSTREAM_VULKAN 3
#define VIOGPU_CAPSET_VENUS 4
#define VIOGPU_CAPSET_NEPTUNE 7

typedef struct _VIOGPU_ADAPTERINFO
{
    ULONGLONG IamVioGPU; // Should be set by driver to VIOGPU_IAM
    struct
    {
        UINT Supports3d : 1;
        UINT HasShmem : 1;
        UINT Reserved : 30;
    } Flags;
    ULONGLONG SupportedCapsetIDs;
    LUID AdapterLuid;
} VIOGPU_ADAPTERINFO;

// ================= ESCAPES
#define VIOGPU_GET_DEVICE_ID         0x000
#define VIOGPU_GET_CUSTOM_RESOLUTION 0x001
#define VIOGPU_GET_CAPS              0x002
#define VIOGPU_GET_PCI_INFO          0x003

#define VIOGPU_RES_INFO              0x100
#define VIOGPU_RES_BUSY              0x101
#define VIOGPU_RES_BLOB_SET_INFO     0x102

#define VIOGPU_CTX_INIT              0x200

#define VIOGPU_BLIT_INIT             0x300

#pragma pack(1)
typedef struct _VIOGPU_DISP_MODE
{
    USHORT XResolution;
    USHORT YResolution;
} VIOGPU_DISP_MODE, *PVIOGPU_DISP_MODE;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PARAM_REQ
{
    ULONG ParamId;
    UINT64 Value;
} VIOGPU_PARAM_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CAPSET_REQ
{
    ULONG CapsetId;
    ULONG Version;
    ULONG Size;
    UCHAR *Capset;
} VIOGPU_CAPSET_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_PCI_INFO_REQ
{
    ULONG Domain;
    ULONG Bus;
    ULONG Dev;
    ULONG Func;
} VIOGPU_PCI_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_INFO_REQ
{
    D3DKMT_HANDLE ResHandle;
    ULONG Id;

    BOOL IsBlob;
    BOOL IsCreated;
    BOOL InfoValid;

    VIOGPU_BLOB_INFO Info;

    ULONG BlobMem;
    ULONGLONG BlobId;
    ULONGLONG Size;
} VIOGPU_RES_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_RES_BUSY_REQ
{
    D3DKMT_HANDLE ResHandle;
    BOOL Wait;
    BOOL IsBusy;
} VIOGPU_RES_BUSY_REQ;
#pragma pack()

#pragma pack(1)
typedef struct {
    D3DKMT_HANDLE ResHandle;
    VIOGPU_BLOB_INFO Info;
} VIOGPU_RES_BLOB_SET_INFO_REQ, *PVIOGPU_RES_BLOB_SET_INFO_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CTX_INIT_REQ
{
    UINT CapsetID;
    UINT NumRings;
    UCHAR DebugName[64];
} VIOGPU_CTX_INIT_REQ;
#pragma pack()

typedef struct _VIOGPU_BLIT_PRESENT VIOGPU_BLIT_PRESENT, *PVIOGPU_BLIT_PRESENT;

#pragma pack(1)
typedef struct _VIOGPU_BLIT_INIT_REQ
{
    HANDLE EventUM;
    HANDLE EventKM;
    PVIOGPU_BLIT_PRESENT pBlitPresent;
} VIOGPU_BLIT_INIT_REQ;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_ESCAPE
{
    USHORT Type;
    USHORT DataLength;
    union {
        ULONG Id;
        VIOGPU_DISP_MODE Resolution;
        VIOGPU_PARAM_REQ Parameter;
        VIOGPU_CAPSET_REQ Capset;
        VIOGPU_PCI_INFO_REQ PciInfo;

        VIOGPU_RES_INFO_REQ ResourceInfo;
        VIOGPU_RES_BUSY_REQ ResourceBusy;
        VIOGPU_RES_BLOB_SET_INFO_REQ BlobInfoSet;

        VIOGPU_CTX_INIT_REQ CtxInit;

        VIOGPU_BLIT_INIT_REQ BlitInit;
    } DUMMYUNIONNAME;
} VIOGPU_ESCAPE, *PVIOGPU_ESCAPE;
#pragma pack()

// ================= CreateResource
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_3D_OPTIONS
{
    ULONG target;
    ULONG format;
    ULONG bind;
    ULONG width;
    ULONG height;
    ULONG depth;
    ULONG array_size;
    ULONG last_level;
    ULONG nr_samples;
    ULONG flags;
} VIOGPU_RESOURCE_3D_OPTIONS;
#pragma pack()

#define VIOGPU_BLOB_MEM_GUEST             0x0001
#define VIOGPU_BLOB_MEM_HOST3D            0x0002
#define VIOGPU_BLOB_MEM_HOST3D_GUEST      0x0003

#define VIOGPU_BLOB_FLAG_USE_MAPPABLE     0x0001
#define VIOGPU_BLOB_FLAG_USE_SHAREABLE    0x0002
//#define VIOGPU_BLOB_FLAG_USE_CROSS_DEVICE 0x0004
#pragma pack(1)
typedef struct _VIOGPU_RESOURCE_BLOB_OPTIONS
{
	ULONG blob_mem;
	ULONG blob_flags;
    ULONGLONG blob_id;
} VIOGPU_RESOURCE_BLOB_OPTIONS;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_CREATE_RESOURCE_EXCHANGE
{
    ULONG magic;
} VIOGPU_CREATE_RESOURCE_EXCHANGE;
#pragma pack()

#define VIOGPU_RESOURCE_TYPE_3D   0
#define VIOGPU_RESOURCE_TYPE_BLOB 1
#pragma pack(1)
typedef struct _VIOGPU_CREATE_ALLOCATION_EXCHANGE
{
    ULONG Type;
    union {
        VIOGPU_RESOURCE_3D_OPTIONS Options3D;
        VIOGPU_RESOURCE_BLOB_OPTIONS OptionsBlob;
    };
    ULONGLONG Size;
} VIOGPU_CREATE_ALLOCATION_EXCHANGE;
#pragma pack()

// ================= BLIT

#pragma pack(1)
struct _VIOGPU_BLIT_PRESENT
{
    struct {
        void *resource;
        RECT rect;
    } src;
    struct {
        VIOGPU_CREATE_ALLOCATION_EXCHANGE alloc;
        VIOGPU_RES_INFO_REQ res_info;
        RECT rect;
    } dst;
};
#pragma pack()

// ================= COMMAND BUFFER
#define VIOGPU_CMD_NOP                0x0
#define VIOGPU_CMD_SUBMIT             0x1 // Submit Command to virgl
#define VIOGPU_CMD_TRANSFER_TO_HOST   0x2 // Transfer resource to host
#define VIOGPU_CMD_TRANSFER_FROM_HOST 0x3 // Transfer resource to host
#define VIOGPU_CMD_MAP_BLOB           0x4 // Map blob resource
#define VIOGPU_CMD_UNMAP_BLOB         0x5 // Unmap blob resource

//#define VIOGPU_CMD_SUBMIT_UM          0x6

// #define VIOGPU_EXECBUF_FENCE_FD_IN  0x01
// #define VIOGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIOGPU_EXECBUF_RING_IDX     0x04
#define VIOGPU_EXECBUF_VIRGL        0x08
// #define VIOGPU_EXECBUF_FLAGS        (VIOGPU_EXECBUF_FENCE_FD_IN | VIOGPU_EXECBUF_FENCE_FD_OUT | VIOGPU_EXECBUF_RING_IDX)

#pragma pack(1)
typedef struct _VIOGPU_COMMAND_HDR
{
    UINT type;
    UINT size;
    UINT flags;
    UINT ring_idx;
} VIOGPU_COMMAND_HDR;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_TRANSFER_CMD
{
    ULONG res_id;

    VIOGPU_BOX box;

    ULONGLONG offset;
    ULONG level;
    ULONG stride;
    ULONG layer_stride;
} VIOGPU_TRANSFER_CMD;
#pragma pack()

#pragma pack(1)
typedef struct _VIOGPU_BEGIN_UM_BLIT_CMD
{
    RECT src, dst;
} VIOGPU_BEGIN_UM_BLIT_CMD;
#pragma pack()

#define BASE_NAMED_OBJECTS    L"\\BaseNamedObjects\\"
#define GLOBAL_OBJECTS        L"Global\\"
#define RESOLUTION_EVENT_NAME L"VioGpuResolutionEvent"
