// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "precomp.h"

#include "IExecutionProvider.h"
#include "ExecutionProvider.h"
#include "PooledUploadHeap.h"
#include "ReadbackHeap.h"
#include "ExecutionContext.h"
#include "BucketizedBufferAllocator.h"
#include "DmlCpuAllocator.h"
#include "MLOperatorAuthorImpl.h"
#include "core/providers/dml/OperatorAuthorHelper/MLOperatorAuthorHelper.h"
#include "core/providers/dml/OperatorAuthorHelper/OperatorHelper.h"
#include "AbiCustomRegistry.h"
#include "GraphPartitioner.h"
#include "core/graph/indexed_sub_graph.h"
#include "core/framework/compute_capability.h"
#include "core/framework/fallback_cpu_capability.h"
#include "DmlCommittedResourceWrapper.h"
#include "DmlBufferRegion.h"
#include "DmlManagedBufferRegion.h"
#include "DmlBfcAllocator.h"
#include "DmlGpuAllocator.h"

#ifdef ERROR
#undef ERROR
#endif
#include "core/session/inference_session.h"
#define ERROR 0

#include "core/session/onnxruntime_c_api.h"
#include <wil/wrl.h>
#ifndef _GAMING_XBOX
#include <dxgi1_6.h>
#endif

#define ENABLE_GRAPH_COMPILATION

using namespace Windows::AI::MachineLearning::Adapter;

namespace Dml
{
    using namespace onnxruntime::common;

    ExecutionProvider::~ExecutionProvider()
    {
        if (m_impl)
        {
            m_impl->Close();
        }
    }

    static void CreateDmlKernelRegistry(
        _Out_ std::shared_ptr<onnxruntime::KernelRegistry>* registry,
        _Out_ std::shared_ptr<const InternalRegistrationInfoMap>* internalRegInfoMap)
    {
        ComPtr<AbiCustomRegistry> abiRegistry = wil::MakeOrThrow<AbiCustomRegistry>();
        Dml::RegisterDmlOperators(abiRegistry.Get());

        assert(abiRegistry->GetRegistries().size() == 1);

        auto customRegistry = *abiRegistry->GetRegistries().begin();
        *registry = customRegistry->GetKernelRegistry();
        *internalRegInfoMap = abiRegistry->GetInternalRegInfoMap();
    }

    ExecutionProvider::ExecutionProvider(
        IDMLDevice* dmlDevice,
        ID3D12CommandQueue* commandQueue,
        bool enableMetacommands) :
            IExecutionProvider(onnxruntime::kDmlExecutionProvider)
    {
        D3D12_COMMAND_LIST_TYPE queueType = commandQueue->GetDesc().Type;
        if (queueType != D3D12_COMMAND_LIST_TYPE_DIRECT && queueType != D3D12_COMMAND_LIST_TYPE_COMPUTE)
        {
            // DML requires either DIRECT or COMPUTE command queues.
            ORT_THROW_HR(E_INVALIDARG);
        }

        ComPtr<ID3D12Device> device;
        GRAPHICS_THROW_IF_FAILED(commandQueue->GetDevice(IID_GRAPHICS_PPV_ARGS(device.GetAddressOf())));

        m_impl = wil::MakeOrThrow<ExecutionProviderImpl>(dmlDevice, device.Get(), commandQueue, enableMetacommands);

        // Register the allocators with ORT, through concrete ORT methods on the IExecutionProvider base class
        InsertAllocator(m_impl->GetGpuAllocator());
        InsertAllocator(m_impl->GetCpuInputAllocator());
        InsertAllocator(m_impl->GetCpuOutputAllocator());
    }

    std::vector<std::unique_ptr<onnxruntime::ComputeCapability>>
    ExecutionProvider::GetCapability(
        const onnxruntime::GraphViewer& graph,
        const onnxruntime::IExecutionProvider::IKernelLookup& kernel_lookup) const
    {
#ifdef ENABLE_GRAPH_COMPILATION
        return m_impl->GetCapability(graph, kernel_lookup);
#else
        return onnxruntime::IExecutionProvider::GetCapability(graph, kernel_lookup);
#endif
    }

    void ExecutionProviderImpl::Close()
    {
        m_context->Close();
    }

    void ExecutionProviderImpl::WaitForOutstandingWork()
    {
        Flush();
        m_context->GetCurrentCompletionEvent().WaitForSignal();
    }

    HRESULT __stdcall ExecutionProviderImpl::AllocatePooledResource(
        size_t size,
        DmlManagedBufferRegion** managedBufferRegion
    ) const noexcept
    {
        ORT_TRY
        {
        void* opaqueData = m_gpuAllocator->Alloc(size);
        auto bufferRegion = m_gpuAllocator->CreateManagedBufferRegion(opaqueData, size);
        bufferRegion.CopyTo(managedBufferRegion);
        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    D3D12BufferRegion ExecutionProviderImpl::GetBufferForTensor(IMLOperatorTensor* tensor) const
    {
        MLOperatorTensor mlOperatorTensor(tensor);
        void* data = mlOperatorTensor.GetByteData();
        auto sizeInBytes = mlOperatorTensor.GetUnalignedTensorByteSize();
        return m_gpuAllocator->CreateBufferRegion(data, sizeInBytes);
    }

    ID3D12Resource* __stdcall ExecutionProviderImpl::DecodeResource(IMLOperatorTensor* tensor) const noexcept
    {
        ORT_TRY
        {
            return GetBufferForTensor(tensor).ResourceInUavState();
        }
        ORT_CATCH_GENERIC
        {
            return nullptr;
        }
    }

// ORT release pipelines agent pools do not have 19H1 SDK installed which defines D3D_FEATURE_LEVEL_1_0_CORE.
// Once ORT/WinML github project can be built with VS2019, we can update these pools to use install the 19H1 SDK
// using the command line installer tool with VS2019
// Task 24384515: Update ORT AIInfra release agent pool to install 19H1 SDK on VM bootstrap
#define D3D_FEATURE_LEVEL_1_0_CORE_PRIVATE ((D3D_FEATURE_LEVEL)0x1000)

    ExecutionProviderImpl::ExecutionProviderImpl(IDMLDevice* dmlDevice, ID3D12Device* d3d12Device, ID3D12CommandQueue* queue, bool enableMetacommands)
        : m_d3d12Device(d3d12Device),
          m_dmlDevice(dmlDevice),
          m_areMetacommandsEnabled(enableMetacommands)
    {

        D3D12_FEATURE_DATA_FEATURE_LEVELS featureLevels = {};

        D3D_FEATURE_LEVEL featureLevelsList[] = {
            D3D_FEATURE_LEVEL_1_0_CORE_PRIVATE,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_12_1
        };

        featureLevels.NumFeatureLevels = ARRAYSIZE(featureLevelsList);
        featureLevels.pFeatureLevelsRequested = featureLevelsList;
        ORT_THROW_IF_FAILED(d3d12Device->CheckFeatureSupport(
            D3D12_FEATURE_FEATURE_LEVELS,
            &featureLevels,
            sizeof(featureLevels)
            ));

        m_isMcdmDevice = (featureLevels.MaxSupportedFeatureLevel == D3D_FEATURE_LEVEL_1_0_CORE_PRIVATE);

        m_context = std::make_shared<ExecutionContext>(m_d3d12Device.Get(), m_dmlDevice.Get(), queue);

        auto subAllocator = std::make_shared<BucketizedBufferAllocator>(
            m_d3d12Device.Get(),
            m_context,
            queue,
            CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT),
            D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

        // Create a BFC allocator that encapsulates our allocator
        onnxruntime::AllocatorCreationInfo memoryInfo(
            [subAllocator](OrtDevice::DeviceId id) {
                return std::make_unique<DmlBfcAllocator>(subAllocator.get());
            });

        m_bfcAllocator = onnxruntime::CreateAllocator(memoryInfo);

        // Wrap the BFC allocator into our own allocator
        m_gpuAllocator = std::make_shared<DmlGpuAllocator>(m_bfcAllocator.get(), subAllocator);

        m_context->SetAllocator(m_gpuAllocator);

        m_uploadHeap = std::make_unique<PooledUploadHeap>(m_d3d12Device.Get(), m_context);
        m_readbackHeap = std::make_unique<ReadbackHeap>(m_d3d12Device.Get(), m_context);

        // CPU Allocator used to create buffers for the MemcpyFromHost, Shape and Size operators.
        m_cpuInputAllocator = std::make_shared<DmlCpuAllocator>(OrtMemType::OrtMemTypeCPUInput);
        m_cpuOutputAllocator = std::make_shared<DmlCpuAllocator>(OrtMemType::OrtMemTypeCPUOutput);

        CreateDmlKernelRegistry(&m_kernelRegistry, &m_internalRegInfoMap);
    }

    HRESULT __stdcall ExecutionProviderImpl::GetD3DDevice(_COM_Outptr_ ID3D12Device** d3dDevice) const noexcept
    {
        m_d3d12Device.CopyTo(d3dDevice);
        _Analysis_assume_(*d3dDevice != nullptr);
        return S_OK;
    }

    HRESULT __stdcall ExecutionProviderImpl::GetDmlDevice(_COM_Outptr_ IDMLDevice** dmlDevice) const noexcept
    {
        m_dmlDevice.CopyTo(dmlDevice);
        _Analysis_assume_(*dmlDevice != nullptr);
        return S_OK;
    }

    HRESULT __stdcall ExecutionProviderImpl::ExecuteCommandList(
        ID3D12GraphicsCommandList* commandList,
        _Outptr_ ID3D12Fence** fence,
        _Out_ uint64_t* completionValue
        ) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);
        m_context->ExecuteCommandList(commandList, fence, completionValue);

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT __stdcall ExecutionProviderImpl::AddUAVBarrier() const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        m_context->AddUAVBarrier();

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT __stdcall ExecutionProviderImpl::InitializeOperator(
        IDMLCompiledOperator* op,
        _In_opt_ const DML_BUFFER_BINDING* persistentResourceBinding,
        gsl::span<const DML_BUFFER_BINDING> inputBindings
        ) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        bool hasInputsToBind = false;
        std::vector<DML_BUFFER_BINDING> inputBufferBindings(inputBindings.size());

        for (size_t i = 0; i < inputBindings.size(); i++)
        {
            if (inputBindings[i].Buffer)
            {
                hasInputsToBind = true;
                inputBufferBindings[i] = { inputBindings[i].Buffer, inputBindings[i].Offset, inputBindings[i].SizeInBytes };
            }
        }

        DML_BINDING_DESC persistentResourceBindingDesc =
            persistentResourceBinding
            ? DML_BINDING_DESC{ DML_BINDING_TYPE_BUFFER, persistentResourceBinding }
            : DML_BINDING_DESC{ DML_BINDING_TYPE_NONE, nullptr };

        DML_BUFFER_ARRAY_BINDING inputBufferArrayDesc;
        inputBufferArrayDesc.BindingCount = gsl::narrow_cast<uint32_t>(inputBufferBindings.size());
        inputBufferArrayDesc.Bindings = inputBufferBindings.data();

        DML_BINDING_DESC inputArrayBindingDesc = hasInputsToBind ?
            DML_BINDING_DESC{ DML_BINDING_TYPE_BUFFER_ARRAY, &inputBufferArrayDesc } :
            DML_BINDING_DESC{ DML_BINDING_TYPE_NONE, nullptr };

        m_context->InitializeOperator(
            op,
            persistentResourceBindingDesc,
            inputArrayBindingDesc);

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT __stdcall ExecutionProviderImpl::ExecuteOperator(
        IDMLCompiledOperator* op,
        _In_opt_ const DML_BUFFER_BINDING* persistentResourceBinding,
        gsl::span<IMLOperatorTensor*> inputTensors,
        gsl::span<IMLOperatorTensor*> outputTensors
        ) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        std::vector<uint32_t> shape;

        for (IMLOperatorTensor* tensor : inputTensors)
        {
            if (tensor)
            {
                shape.resize(tensor->GetDimensionCount());
                ORT_THROW_IF_FAILED(tensor->GetShape(tensor->GetDimensionCount(), shape.data()));

                if (OperatorHelper::ContainsEmptyDimensions(shape))
                {
                    return S_OK;
                }
            }
        }

        for (IMLOperatorTensor* tensor : outputTensors)
        {
            if (tensor)
            {
                shape.resize(tensor->GetDimensionCount());
                ORT_THROW_IF_FAILED(tensor->GetShape(tensor->GetDimensionCount(), shape.data()));

                if (OperatorHelper::ContainsEmptyDimensions(shape))
                {
                    return S_OK;
                }
            }
        }

        auto FillBindings = [this](auto& bufferBindings, auto& bindingDescs, auto& tensors)
        {
            for (IMLOperatorTensor* tensor : tensors)
            {
                if (tensor)
                {
                    assert(tensor->IsDataInterface());
                    auto bufferRegion = GetBufferForTensor(tensor);
                    bufferBindings.push_back(bufferRegion.GetBufferBinding());
                    bindingDescs.push_back({ DML_BINDING_TYPE_BUFFER, &bufferBindings.back() });
                }
                else
                {
                    bufferBindings.push_back({ nullptr, 0, 0 });
                    bindingDescs.push_back({ DML_BINDING_TYPE_NONE, nullptr });
                }
            }
        };

        std::vector<DML_BUFFER_BINDING> inputBufferBindings;
        inputBufferBindings.reserve(inputTensors.size());
        std::vector<DML_BINDING_DESC> inputBindings;
        inputBindings.reserve(inputTensors.size());
        FillBindings(inputBufferBindings, inputBindings, inputTensors);

        std::vector<DML_BUFFER_BINDING> outputBufferBindings;
        outputBufferBindings.reserve(outputTensors.size());
        std::vector<DML_BINDING_DESC> outputBindings;
        outputBindings.reserve(outputTensors.size());
        FillBindings(outputBufferBindings, outputBindings, outputTensors);

        ORT_THROW_IF_FAILED(ExecuteOperator(op, persistentResourceBinding, inputBindings, outputBindings));

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT __stdcall ExecutionProviderImpl::ExecuteOperator(
        IDMLCompiledOperator* op,
        _In_opt_ const DML_BUFFER_BINDING* persistentResourceBinding,
        gsl::span<DML_BINDING_DESC> inputTensors,
        gsl::span<DML_BINDING_DESC> outputTensors
        ) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        DML_BINDING_DESC persistentResourceBindingDesc =
            persistentResourceBinding
            ? DML_BINDING_DESC{ DML_BINDING_TYPE_BUFFER, persistentResourceBinding }
            : DML_BINDING_DESC{ DML_BINDING_TYPE_NONE, nullptr };

        m_context->ExecuteOperator(
            op,
            persistentResourceBindingDesc,
            inputTensors,
            outputTensors);

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    static gsl::span<const std::byte> AsByteSpan(const void* data, size_t sizeInBytes)
    {
        return gsl::make_span(static_cast<const std::byte*>(data), sizeInBytes);
    }

    static gsl::span<std::byte> AsByteSpan(void* data, size_t sizeInBytes)
    {
        return gsl::make_span(static_cast<std::byte*>(data), sizeInBytes);
    }

    HRESULT __stdcall ExecutionProviderImpl::CopyTensor(IMLOperatorTensor* dst, IMLOperatorTensor* src) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        const size_t sourceSizeInBytes = ComputeByteSizeFromTensor(*src);
        const size_t dataSizeInBytes = ComputeByteSizeFromTensor(*dst);
        ORT_THROW_HR_IF(E_INVALIDARG, dataSizeInBytes != sourceSizeInBytes); // Tensors must be the same size

        if (dataSizeInBytes == 0)
        {
            return S_OK;
        }

        if (src->IsCpuData() && !dst->IsCpuData())
        {
            //
            // CPU -> GPU copy (upload)
            //
            auto dstBufferRegion = GetBufferForTensor(dst);

            ID3D12Resource* dstData = dstBufferRegion.ResourceInCopyDstState() == nullptr
                ? dstBufferRegion.ResourceInUavState()
                : dstBufferRegion.ResourceInCopyDstState();

            const auto dstState = dstBufferRegion.ResourceInCopyDstState() == nullptr
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_COPY_DEST;

            const uint64_t dstOffset = dstBufferRegion.Offset();
            m_uploadHeap->BeginUploadToGpu(dstData, dstOffset, dstState, AsByteSpan(src->GetData(), dataSizeInBytes));
        }
        else if (!src->IsCpuData() && dst->IsCpuData())
        {
            //
            // GPU -> CPU copy (readback)
            //
            auto srcBufferRegion = GetBufferForTensor(src);

            ID3D12Resource* srcData = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? srcBufferRegion.ResourceInUavState()
                : srcBufferRegion.ResourceInCopySrcState();

            const auto srcState = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_COPY_SOURCE;

            const uint64_t srcOffset = srcBufferRegion.Offset();
            m_readbackHeap->ReadbackFromGpu(AsByteSpan(dst->GetData(), dataSizeInBytes), srcData, srcOffset, srcState);
        }
        else if (!src->IsCpuData() && !dst->IsCpuData())
        {
            //
            // GPU -> GPU copy
            //
            auto srcBufferRegion = GetBufferForTensor(src);

            ID3D12Resource* srcData = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? srcBufferRegion.ResourceInUavState()
                : srcBufferRegion.ResourceInCopySrcState();

            const auto srcState = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_COPY_SOURCE;

            auto dstBufferRegion = GetBufferForTensor(dst);

            ID3D12Resource* dstData = dstBufferRegion.ResourceInCopyDstState() == nullptr
                ? dstBufferRegion.ResourceInUavState()
                : dstBufferRegion.ResourceInCopyDstState();

            const auto dstState = dstBufferRegion.ResourceInCopyDstState() == nullptr
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_COPY_DEST;

            m_context->CopyBufferRegion(dstData, 0, dstState, srcData, 0, srcState, dataSizeInBytes);
        }
        else
        {
            // CPU -> CPU copies not supported
            ORT_THROW_HR(E_INVALIDARG);
        }

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT STDMETHODCALLTYPE ExecutionProviderImpl::FillTensorWithPattern(
        IMLOperatorTensor* dst,
        gsl::span<const std::byte> rawValue // Data type agnostic rawValue, treated as raw bits
        ) const noexcept
    {
        ORT_TRY
        {
        auto mlTensor = MLOperatorTensor(dst).GetDataInterface();
        if (mlTensor != nullptr)
        {
            auto dstBufferRegion = GetBufferForTensor(dst);
            m_context->FillBufferWithPattern(dstBufferRegion.ResourceInUavState(), dstBufferRegion.Offset(), rawValue);
        }

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    HRESULT __stdcall ExecutionProviderImpl::UploadToResource(ID3D12Resource* dstData, const void* srcData, uint64_t srcDataSize) const noexcept
    {
        ORT_TRY
        {
        assert(!m_closed);

        m_uploadHeap->BeginUploadToGpu(dstData, 0, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, AsByteSpan(srcData, static_cast<size_t>(srcDataSize)));

        return S_OK;
        }
        ORT_CATCH_RETURN
    }

    uint32_t ExecutionProviderImpl::GetSupportedDeviceDataTypeMask() const
    {
        // The DML provider registers all supported kernels up-front regardless of actual device capability,
        // but this is problematic later when executing the graph because DirectML will fail to create
        // the operator, and by that late phase, it's long past too late to recover. So, this function queries
        // the actual type capabilities so the partitioner may assigns nodes to the CPU if the GPU cannot
        // handle them, similar to the fallback in CUDAExecutionProvider::GetCapability for certain RNN/GRU/Conv
        // attributes.

        return Dml::GetSupportedDeviceDataTypeMask(m_dmlDevice.Get());
    }

    bool TryGetTensorDataType(
        const onnxruntime::NodeArg& nodeArg,
        _Out_ MLOperatorTensorDataType* onnxElementType
    )
    {
        *onnxElementType = MLOperatorTensorDataType::Undefined;

        const ::onnx::TypeProto* typeProto = nodeArg.TypeAsProto();
        if (typeProto != nullptr && typeProto->has_tensor_type())
        {
            const ::onnx::TypeProto_Tensor& tensorTypeProto = typeProto->tensor_type();
            if (tensorTypeProto.has_elem_type())
            {
                *onnxElementType = static_cast<MLOperatorTensorDataType>(tensorTypeProto.elem_type());
                return true;
            }
        }

        return false;
    }

    bool DoesNodeContainSupportedDataTypes(
        const onnxruntime::Node& node,
        _In_opt_ const InternalRegistrationInfo* regInfo,
        uint32_t supportedDeviceDataTypeMask // Each bit corresponds to each DML_TENSOR_DATA_TYPE.
        )
    {
        std::vector<onnxruntime::NodeArg const*> constantCpuInputs;

        if (regInfo != nullptr)
        {
            // Collect the list of CPU-bound input tensors, needed when checking 64-bit fallback
            // or for other data types like int-8 which may be supported for CPU inputs but not
            // GPU inputs.
            auto inputDefinitions = node.InputDefs();
            for (uint32_t i : regInfo->requiredConstantCpuInputs)
            {
                if (i < inputDefinitions.size())
                {
                    constantCpuInputs.push_back(inputDefinitions[i]);
                }
            }
        }

        // Assume data types are supported until proven otherwise.
        bool nodeContainsSupportedDataTypes = true;

        // Callback to check each node's data type against registered operator support.
        std::function<void(const onnxruntime::NodeArg& nodeArg, bool isInput)> nodeCallback = [&](const onnxruntime::NodeArg& nodeArg, bool isInput) -> void
        {
            // Get the tensor element data type for this node, comparing against what the device actually supports.
            // Use the enumeration from the proto instead of nodeArg.Type() which returns a string.

            // Reject node if undefined data type or non-tensor, as DML cannot handle it.
            MLOperatorTensorDataType onnxElementType;
            if (!TryGetTensorDataType(nodeArg, &onnxElementType))
            {
                // We shouldn't have arrived here because (1) no DML operators should have been
                // registered which use non-tensor types (2) ONNX validation should have already
                // been done, checking for the right kind of inputs and attributes. In theory,
                // this branch could be reached with a bad custom operator or malformed file. If
                // a legitimate case reaches here and DML needs to support a new input/output type
                // besides tensors, then remove the assert.
                assert(false);
                nodeContainsSupportedDataTypes = false;
                return;
            }

            // Reject node for unknown DML data types.
            DML_TENSOR_DATA_TYPE dmlElementType = GetDmlDataTypeFromMlDataTypeNoThrow(onnxElementType);
            if (dmlElementType == DML_TENSOR_DATA_TYPE_UNKNOWN)
            {
                nodeContainsSupportedDataTypes = false;
                return;
            }

            // Succeed if the tensor is CPU-bound, as the CPU-side reading code is generic enough
            // to handle multiple types regardless of GPU capability (typically these are just
            // scalars or simple 1D arrays).
            bool isConstantCpuInput = isInput && std::find(constantCpuInputs.begin(), constantCpuInputs.end(), &nodeArg) != constantCpuInputs.end();
            if (isConstantCpuInput)
            {
                // Leave nodeContainsSupportedDataTypes alone.
                return;
            }

            bool isDataTypeSupported = (1 << dmlElementType) & supportedDeviceDataTypeMask;

            // Reject node if the data type is unsupported by the device.
            if (!isDataTypeSupported)
            {
                nodeContainsSupportedDataTypes = false;
                return;
            }

            // Otherwise the node supports the tensor data type.
        };

        // Check whether the node uses any data types which are unsupported by the device.
        node.ForEachDef(nodeCallback);

        return nodeContainsSupportedDataTypes;
    }

    bool ExecutionProviderImpl::IsNodeSupportedByDml(
        const onnxruntime::Node& node,
        const onnxruntime::IExecutionProvider::IKernelLookup& kernel_lookup,
        uint32_t supportedDeviceDataTypeMask // Each bit corresponds to each DML_TENSOR_DATA_TYPE.
        ) const
    {
        const onnxruntime::KernelCreateInfo* createInfo = kernel_lookup.LookUpKernel(node);
        if (!createInfo)
        {
            return false;
        }

        auto regInfoIter = m_internalRegInfoMap->find(createInfo->kernel_def.get());
        std::shared_ptr<InternalRegistrationInfo> internalRegInfo;
        if (regInfoIter != m_internalRegInfoMap->end())
        {
            internalRegInfo = regInfoIter->second;
            if (internalRegInfo->supportQuery && !internalRegInfo->supportQuery(node))
            {
                return false;
            }
        }

        // Check whether the node uses any data types which are unsupported by the device.
        if (!DoesNodeContainSupportedDataTypes(node, internalRegInfo.get(), supportedDeviceDataTypeMask))
        {
            return false;
        }

        return true;
    }

    std::vector<std::unique_ptr<onnxruntime::ComputeCapability>>
    ExecutionProviderImpl::GetCapability(
        const onnxruntime::GraphViewer& graph,
        const onnxruntime::IExecutionProvider::IKernelLookup& kernel_lookup) const
    {
        uint32_t deviceDataTypeMask = GetSupportedDeviceDataTypeMask(); // Each bit corresponds to each DML_TENSOR_DATA_TYPE.

        std::vector<std::unique_ptr<onnxruntime::ComputeCapability>> result;

        // Get the list of node indices in toplogical order, so nodes are visited before
        // downstream nodes consuming them.
        const std::vector<onnxruntime::NodeIndex>& toplogicalOrder = graph.GetNodesInTopologicalOrder();

        std::vector<onnxruntime::NodeIndex> tentativeNodes;
        tentativeNodes.reserve(toplogicalOrder.size());

        for (onnxruntime::NodeIndex nodeIndex : toplogicalOrder)
        {
            const onnxruntime::Node& node = *graph.GetNode(nodeIndex);
            const auto* kernelInfo = kernel_lookup.LookUpKernel(node);
            if (kernelInfo != nullptr)
            {
                tentativeNodes.push_back(nodeIndex);
            }
        }

        // Get the list of nodes that should stay on the CPU
        auto cpuPreferredNodes = GetCpuPreferredNodes(graph, kernel_lookup, tentativeNodes);

        for (size_t nodeIndex : toplogicalOrder)
        {
            const onnxruntime::Node& node = *graph.GetNode(nodeIndex);
            if (IsNodeSupportedByDml(node, kernel_lookup, deviceDataTypeMask)
                && cpuPreferredNodes.find(nodeIndex) == cpuPreferredNodes.end())
            {
                std::unique_ptr<onnxruntime::IndexedSubGraph> subGraph = std::make_unique<onnxruntime::IndexedSubGraph>();
                subGraph->nodes = {nodeIndex};
                result.push_back(std::make_unique<onnxruntime::ComputeCapability>(std::move(subGraph)));
            }
        }
        return result;
    }

    bool IsGpuTensor(const onnxruntime::Tensor& tensor)
    {
        return strcmp(tensor.Location().name, onnxruntime::CPU) &&
            !(tensor.Location().mem_type == ::OrtMemType::OrtMemTypeCPUOutput || tensor.Location().mem_type == ::OrtMemType::OrtMemTypeCPUInput);
    }

    Status ExecutionProviderImpl::CopyTensor(const onnxruntime::Tensor& src, onnxruntime::Tensor& dst) const
    {
        assert(!m_closed);

        auto provider = const_cast<ExecutionProviderImpl*>(this);

        TensorWrapper destInternal(
            &dst,
            IsGpuTensor(dst),
            provider,
            true);

        TensorWrapper srcInternal(
            const_cast<onnxruntime::Tensor*>(&src),
            IsGpuTensor(src),
            provider,
            true);

        ORT_THROW_IF_FAILED(CopyTensor(&destInternal, &srcInternal));

        return onnxruntime::common::Status::OK();
    }

    Status ExecutionProviderImpl::CopyTensors(const std::vector<onnxruntime::IDataTransfer::SrcDstPair>& src_dst_pairs) const
    {
        // Source and destination for batched GPU -> CPU copies
        std::vector<ID3D12Resource*> srcDatas;
        srcDatas.reserve(src_dst_pairs.size());

        std::vector<D3D12_RESOURCE_STATES> srcStates;
        srcStates.reserve(src_dst_pairs.size());

        std::vector<uint64_t> srcOffsets;
        srcOffsets.reserve(src_dst_pairs.size());

        std::vector<void*> dstDatas;
        dstDatas.reserve(src_dst_pairs.size());

        std::vector<uint32_t> dataSizesInBytes;
        dataSizesInBytes.reserve(src_dst_pairs.size());

        assert(!m_closed);
        auto provider = const_cast<ExecutionProviderImpl*>(this);

        for (uint32_t i = 0; i < src_dst_pairs.size(); ++i)
        {
            // This batching implementation only handles GPU -> CPU copies.  Other copies do not require synchronization
            // and are batched across multiple calls to CopyTensor.
            if (!IsGpuTensor(src_dst_pairs[i].src) || IsGpuTensor(src_dst_pairs[i].dst))
            {
                ORT_RETURN_IF_ERROR(CopyTensor(src_dst_pairs[i].src, src_dst_pairs[i].dst));
                continue;
            }

            TensorWrapper srcWrapper = TensorWrapper(
                const_cast<onnxruntime::Tensor*>(&src_dst_pairs[i].src.get()),
                true,
                provider,
                true);

            TensorWrapper dstWrapper = TensorWrapper(
                &src_dst_pairs[i].dst.get(),
                false,
                provider,
                true);

            const size_t dataSizeInBytes = ComputeByteSizeFromTensor(dstWrapper);
            ORT_THROW_HR_IF(E_INVALIDARG, dataSizeInBytes != ComputeByteSizeFromTensor(srcWrapper)); // Tensors must be the same size

            if (dataSizeInBytes == 0)
            {
                return onnxruntime::common::Status::OK();
            }

            dataSizesInBytes.push_back(static_cast<uint32_t>(ComputeByteSizeFromTensor(dstWrapper)));
            ORT_THROW_HR_IF(E_INVALIDARG, dataSizesInBytes[i] != ComputeByteSizeFromTensor(srcWrapper)); // Tensors must be the same size

            dstDatas.push_back(dstWrapper.GetData());

            auto srcBufferRegion = GetBufferForTensor(&srcWrapper);

            ID3D12Resource* srcData = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? srcBufferRegion.ResourceInUavState()
                : srcBufferRegion.ResourceInCopySrcState();

            const auto srcState = srcBufferRegion.ResourceInCopySrcState() == nullptr
                ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                : D3D12_RESOURCE_STATE_COPY_SOURCE;

            srcDatas.push_back(srcData);
            srcStates.push_back(srcState);
            srcOffsets.push_back(srcBufferRegion.Offset());
        }

        // Performs a blocking call to synchronize and read back data from the GPU into the destination buffer
        m_readbackHeap->ReadbackFromGpu(dstDatas, dataSizesInBytes, srcDatas, srcOffsets, srcStates);

        return onnxruntime::common::Status::OK();
    }

    void __stdcall ExecutionProviderImpl::Flush() const
    {
        assert(!m_closed);
        m_context->Flush();
    }

    void ExecutionProviderImpl::SetDefaultRoundingMode(AllocatorRoundingMode roundingMode)
    {
        m_gpuAllocator->SetDefaultRoundingMode(roundingMode);
    }

    void ExecutionProviderImpl::ReleaseCompletedReferences()
    {
         m_context->ReleaseCompletedReferences();
    }

    void ExecutionProviderImpl::QueueReference(IUnknown* object)
    {
        assert(!m_closed);
        m_context->QueueReference(object);
    }

    void ExecutionProviderImpl::GetABIDataInterface(void* data, IUnknown** abiData) const
    {
        assert(!m_closed);
        auto uavResource = m_gpuAllocator->GetAllocationInfo(data)->GetUavResource();
        uavResource->AddRef();
        *abiData = uavResource;
    }

    void ExecutionProviderImpl::GetManagedBufferRegion(void* data, uint64_t size, DmlManagedBufferRegion** abiData) const
    {
        auto managedBufferRegion = m_gpuAllocator->CreateManagedBufferRegion(data, size);
        ORT_THROW_IF_FAILED(managedBufferRegion.CopyTo(abiData));
    }

    uint64_t ExecutionProviderImpl::TryGetPooledAllocationId(void* data, bool isInternalOperator)
    {
        assert(!isInternalOperator);
        return m_gpuAllocator->GetAllocationInfo(data)->GetPooledResourceId();
    }

    void ExecutionProviderImpl::GetABIExecutionInterfaceAndInvalidateState(
        bool isInternalOperator,
        IUnknown** abiExecutionObject) const
    {
        assert(!m_closed);

        if (isInternalOperator)
        {
            ComPtr<IUnknown> thisPtr = const_cast<IExecutionProvider*>(static_cast<const IExecutionProvider*>(this));
            *abiExecutionObject = thisPtr.Detach();
        }
        else
        {
            ComPtr<ID3D12GraphicsCommandList> commandList;
            m_context->GetCommandListForRecordingAndInvalidateState(commandList.GetAddressOf());
#ifdef _GAMING_XBOX
            ComPtr<GraphicsUnknownWrapper> wrappedCommandList = Microsoft::WRL::Make<GraphicsUnknownWrapper>(commandList.Get());
            *abiExecutionObject = wrappedCommandList.Detach();
#else
            *abiExecutionObject = commandList.Detach();
#endif
        }
    }

    bool ExecutionProviderImpl::TransitionsRequiredForOperator(
        bool isInternalOperator
    )
    {
        // External operators receive resources in Common state, while internal operators receive
        // them in UAV state. Resources are otherwise kept in UAV state (or are promotable to UAV).
        return !isInternalOperator;
    }

    void ExecutionProviderImpl::TransitionResourcesForOperator(
        bool isBeforeOp,
        uint32_t resourceCount,
        IUnknown** resources
    )
    {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(resourceCount);

        for (uint32_t i = 0; i < resourceCount; ++i)
        {
            ComPtr<ID3D12Resource> resource;
            ORT_THROW_IF_FAILED(resources[i]->QueryInterface(resource.GetAddressOf()));

            // Custom operators receive resources in Common state and must return them to Common
            // state when finished.  Resources are otherwise kept in UAV state (or are promotable to UAV).
            barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(
                resource.Get(),
                isBeforeOp ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COMMON,
                isBeforeOp ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_UNORDERED_ACCESS
            ));
        }

        if (!barriers.empty())
        {
            m_context->ResourceBarrier(barriers);
        }
    }

    D3D12_COMMAND_LIST_TYPE __stdcall ExecutionProviderImpl::GetCommandListTypeForQueue() const
    {
        return m_context->GetCommandListTypeForQueue();
    }

    bool __stdcall ExecutionProviderImpl::IsMcdmDevice() const noexcept
    {
        return m_isMcdmDevice;
    }

    bool __stdcall ExecutionProviderImpl::MetacommandsEnabled() const noexcept
    {
        return m_areMetacommandsEnabled;
    }

    std::shared_ptr<const Windows::AI::MachineLearning::Adapter::InternalRegistrationInfoMap>
    ExecutionProviderImpl::GetInternalRegistrationInfoMap() const
    {
        return m_internalRegInfoMap;
    }

    std::shared_ptr<onnxruntime::IAllocator> ExecutionProviderImpl::GetGpuAllocator()
    {
        return m_bfcAllocator;
    }

    std::shared_ptr<onnxruntime::IAllocator> ExecutionProviderImpl::GetCpuInputAllocator()
    {
        return m_cpuInputAllocator;
    }

    std::shared_ptr<onnxruntime::IAllocator> ExecutionProviderImpl::GetCpuOutputAllocator()
    {
        return m_cpuOutputAllocator;
    }


    onnxruntime::common::Status ExecutionProviderImpl::OnSessionInitializationEnd()
    {
        // Flush and trim resources, including staging memory used to upload weights.
        // This reduces memory usage immediately after session creation, and avoids
        // performance impact of deallocation during first evaluation.
        Flush();
        m_context->GetCurrentCompletionEvent().WaitForSignal();
        m_context->ReleaseCompletedReferences();
        m_uploadHeap->Trim();

        return onnxruntime::common::Status::OK();
    }

    std::unique_ptr<onnxruntime::IExecutionProvider> CreateExecutionProvider(
        IDMLDevice* dmlDevice,
        ID3D12CommandQueue* commandQueue,
        bool enableMetacommands)
    {
        return std::make_unique<Dml::ExecutionProvider>(dmlDevice, commandQueue, enableMetacommands);
    }

    ID3D12Resource* GetD3D12ResourceFromAllocation(onnxruntime::IAllocator* allocator, void* ptr)
    {
        Dml::DmlGpuAllocator* pAllocationInfo = static_cast<Dml::DmlGpuAllocator*>(allocator);
        return pAllocationInfo->GetAllocationInfo(ptr)->GetUavResource();
    }

    void FlushContext(onnxruntime::IExecutionProvider* provider)
    {
        ExecutionProvider* dmlexecutionprovider = static_cast<Dml::ExecutionProvider*>(provider);
        dmlexecutionprovider->Flush();
    }

    void SetDefaultRoundingMode(onnxruntime::IExecutionProvider* provider, AllocatorRoundingMode roundingMode)
    {
        ExecutionProvider* dmlexecutionprovider = static_cast<Dml::ExecutionProvider*>(provider);
        dmlexecutionprovider->SetDefaultRoundingMode(roundingMode);
    }

    void ReleaseCompletedReferences(onnxruntime::IExecutionProvider * provider)
    {
        ExecutionProvider* dmlexecutionprovider = static_cast<Dml::ExecutionProvider*>(provider);
        dmlexecutionprovider->ReleaseCompletedReferences();
    }

    onnxruntime::common::Status CopyTensor(
        onnxruntime::IExecutionProvider* provider,
        const onnxruntime::Tensor& src,
        onnxruntime::Tensor& dst
    )
    {
        ExecutionProvider* dmlexecutionprovider = static_cast<Dml::ExecutionProvider*>(provider);
        return dmlexecutionprovider->GetImpl()->CopyTensor(src, dst);
    }

    void* CreateGPUAllocationFromD3DResource(ID3D12Resource* pResource)
    {
        uint64_t pooledResourceId = 0; // Not a pooled resource

        ComPtr<DmlResourceWrapper> resourceWrapper;
        wil::MakeOrThrow<DmlCommittedResourceWrapper>(pResource).As(&resourceWrapper);

        ComPtr<AllocationInfo> allocInfo = wil::MakeOrThrow<AllocationInfo>(nullptr, 0, pooledResourceId, resourceWrapper.Get(), (size_t)pResource->GetDesc().Width);
        return allocInfo.Detach();
    }
    void FreeGPUAllocation(void* ptr)
    {
        ComPtr<AllocationInfo> allocInfo;
        allocInfo.Attach(static_cast<AllocationInfo*>(ptr));
    }

} // namespace Dml
