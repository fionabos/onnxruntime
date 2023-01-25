// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "precomp.h"
#include "DmlGpuAllocator.h"
#include "core/framework/allocator.h"
#include "BucketizedBufferAllocator.h"

namespace Dml
{
    DmlGpuAllocator::DmlGpuAllocator(onnxruntime::IAllocator* bfcAllocator, std::shared_ptr<BucketizedBufferAllocator> subAllocator)
    : onnxruntime::IAllocator(
        OrtMemoryInfo(
            "DML",
            OrtAllocatorType::OrtDeviceAllocator,
            OrtDevice(OrtDevice::GPU, OrtDevice::MemType::DEFAULT, 0)
        )
    ),
    m_bfcAllocator(bfcAllocator),
    m_subAllocator(std::move(subAllocator)) {}

    void* DmlGpuAllocator::Alloc(size_t size_in_bytes)
    {
        return m_bfcAllocator->Alloc(size_in_bytes);
    }

    void DmlGpuAllocator::Free(void* ptr)
    {
        m_bfcAllocator->Free(ptr);
    }

    D3D12BufferRegion DmlGpuAllocator::CreateBufferRegion(const void* ptr, uint64_t size_in_bytes)
    {
        return m_subAllocator->CreateBufferRegion(ptr, size_in_bytes);
    }

    ComPtr<DmlManagedBufferRegion> DmlGpuAllocator::CreateManagedBufferRegion(const void* ptr, uint64_t size_in_bytes)
    {
        return m_subAllocator->CreateManagedBufferRegion(ptr, size_in_bytes);
    }

    AllocationInfo* DmlGpuAllocator::GetAllocationInfo(const void* ptr)
    {
        return m_subAllocator->GetAllocationInfo(ptr);
    }

    void DmlGpuAllocator::SetDefaultRoundingMode(AllocatorRoundingMode roundingMode)
    {
        m_subAllocator->SetDefaultRoundingMode(roundingMode);
    }
} // namespace Dml
