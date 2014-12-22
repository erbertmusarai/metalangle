//
// Copyright (c) 2012 The ANGLE Project Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//

// InputLayoutCache.cpp: Defines InputLayoutCache, a class that builds and caches
// D3D11 input layouts.

#include "libANGLE/renderer/d3d/d3d11/InputLayoutCache.h"
#include "libANGLE/renderer/d3d/d3d11/VertexBuffer11.h"
#include "libANGLE/renderer/d3d/d3d11/Buffer11.h"
#include "libANGLE/renderer/d3d/d3d11/ShaderExecutable11.h"
#include "libANGLE/renderer/d3d/d3d11/formatutils11.h"
#include "libANGLE/renderer/d3d/ProgramD3D.h"
#include "libANGLE/renderer/d3d/VertexDataManager.h"
#include "libANGLE/Program.h"
#include "libANGLE/VertexAttribute.h"

#include "third_party/murmurhash/MurmurHash3.h"

namespace rx
{

static void GetInputLayout(const TranslatedAttribute translatedAttributes[gl::MAX_VERTEX_ATTRIBS],
                           gl::VertexFormat inputLayout[gl::MAX_VERTEX_ATTRIBS])
{
    for (unsigned int attributeIndex = 0; attributeIndex < gl::MAX_VERTEX_ATTRIBS; attributeIndex++)
    {
        const TranslatedAttribute &translatedAttribute = translatedAttributes[attributeIndex];

        if (translatedAttributes[attributeIndex].active)
        {
            inputLayout[attributeIndex] = gl::VertexFormat(*translatedAttribute.attribute,
                                                           translatedAttribute.currentValueType);
        }
    }
}

const unsigned int InputLayoutCache::kMaxInputLayouts = 1024;

InputLayoutCache::InputLayoutCache() : mInputLayoutMap(kMaxInputLayouts, hashInputLayout, compareInputLayouts)
{
    mCounter = 0;
    mDevice = NULL;
    mDeviceContext = NULL;
    mCurrentIL = NULL;
    for (unsigned int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        mCurrentBuffers[i] = NULL;
        mCurrentVertexStrides[i] = -1;
        mCurrentVertexOffsets[i] = -1;
    }
}

InputLayoutCache::~InputLayoutCache()
{
    clear();
}

void InputLayoutCache::initialize(ID3D11Device *device, ID3D11DeviceContext *context)
{
    clear();
    mDevice = device;
    mDeviceContext = context;
    mFeatureLevel = device->GetFeatureLevel();
}

void InputLayoutCache::clear()
{
    for (InputLayoutMap::iterator i = mInputLayoutMap.begin(); i != mInputLayoutMap.end(); i++)
    {
        SafeRelease(i->second.inputLayout);
    }
    mInputLayoutMap.clear();
    markDirty();
}

void InputLayoutCache::markDirty()
{
    mCurrentIL = NULL;
    for (unsigned int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        mCurrentBuffers[i] = NULL;
        mCurrentVertexStrides[i] = -1;
        mCurrentVertexOffsets[i] = -1;
    }
}

gl::Error InputLayoutCache::applyVertexBuffers(TranslatedAttribute attributes[gl::MAX_VERTEX_ATTRIBS],
                                               gl::Program *program)
{
    ProgramD3D *programD3D = ProgramD3D::makeProgramD3D(program->getImplementation());

    int sortedSemanticIndices[gl::MAX_VERTEX_ATTRIBS];
    programD3D->sortAttributesByLayout(attributes, sortedSemanticIndices);

    if (!mDevice || !mDeviceContext)
    {
        return gl::Error(GL_OUT_OF_MEMORY, "Internal input layout cache is not initialized.");
    }

    InputLayoutKey ilKey = { 0 };

    static const char* semanticName = "TEXCOORD";

    unsigned int firstIndexedElement = gl::MAX_VERTEX_ATTRIBS;
    unsigned int firstInstancedElement = gl::MAX_VERTEX_ATTRIBS;

    for (unsigned int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        if (attributes[i].active)
        {
            D3D11_INPUT_CLASSIFICATION inputClass = attributes[i].divisor > 0 ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;

            gl::VertexFormat vertexFormat(*attributes[i].attribute, attributes[i].currentValueType);
            const d3d11::VertexFormat &vertexFormatInfo = d3d11::GetVertexFormatInfo(vertexFormat, mFeatureLevel);

            // Record the type of the associated vertex shader vector in our key
            // This will prevent mismatched vertex shaders from using the same input layout
            GLint attributeSize;
            program->getActiveAttribute(ilKey.elementCount, 0, NULL, &attributeSize, &ilKey.elements[ilKey.elementCount].glslElementType, NULL);

            ilKey.elements[ilKey.elementCount].desc.SemanticName = semanticName;
            ilKey.elements[ilKey.elementCount].desc.SemanticIndex = sortedSemanticIndices[i];
            ilKey.elements[ilKey.elementCount].desc.Format = vertexFormatInfo.nativeFormat;
            ilKey.elements[ilKey.elementCount].desc.InputSlot = i;
            ilKey.elements[ilKey.elementCount].desc.AlignedByteOffset = 0;
            ilKey.elements[ilKey.elementCount].desc.InputSlotClass = inputClass;
            ilKey.elements[ilKey.elementCount].desc.InstanceDataStepRate = attributes[i].divisor;

            if (inputClass == D3D11_INPUT_PER_VERTEX_DATA && firstIndexedElement == gl::MAX_VERTEX_ATTRIBS)
            {
                firstIndexedElement = ilKey.elementCount;
            }
            else if (inputClass == D3D11_INPUT_PER_INSTANCE_DATA && firstInstancedElement == gl::MAX_VERTEX_ATTRIBS)
            {
                firstInstancedElement = ilKey.elementCount;
            }

            ilKey.elementCount++;
        }
    }

    // On 9_3, we must ensure that slot 0 contains non-instanced data.
    // If slot 0 currently contains instanced data then we swap it with a non-instanced element.
    // Note that instancing is only available on 9_3 via ANGLE_instanced_arrays, since 9_3 doesn't support OpenGL ES 3.0.
    // As per the spec for ANGLE_instanced_arrays, not all attributes can be instanced simultaneously, so a non-instanced element must exist.
    ASSERT(!(mFeatureLevel <= D3D_FEATURE_LEVEL_9_3 && firstInstancedElement == gl::MAX_VERTEX_ATTRIBS));
    bool moveFirstIndexedIntoSlotZero = mFeatureLevel <= D3D_FEATURE_LEVEL_9_3 && firstInstancedElement == 0 && firstIndexedElement != gl::MAX_VERTEX_ATTRIBS;

    if (moveFirstIndexedIntoSlotZero)
    {
        ilKey.elements[firstInstancedElement].desc.InputSlot = ilKey.elements[firstIndexedElement].desc.InputSlot;
        ilKey.elements[firstIndexedElement].desc.InputSlot = 0;
    }

    ID3D11InputLayout *inputLayout = NULL;

    InputLayoutMap::iterator keyIter = mInputLayoutMap.find(ilKey);
    if (keyIter != mInputLayoutMap.end())
    {
        inputLayout = keyIter->second.inputLayout;
        keyIter->second.lastUsedTime = mCounter++;
    }
    else
    {
        gl::VertexFormat shaderInputLayout[gl::MAX_VERTEX_ATTRIBS];
        GetInputLayout(attributes, shaderInputLayout);
        ProgramD3D *programD3D = ProgramD3D::makeProgramD3D(program->getImplementation());

        ShaderExecutable *shader = NULL;
        gl::Error error = programD3D->getVertexExecutableForInputLayout(shaderInputLayout, &shader, nullptr);
        if (error.isError())
        {
            return error;
        }

        ShaderExecutable *shader11 = ShaderExecutable11::makeShaderExecutable11(shader);

        D3D11_INPUT_ELEMENT_DESC descs[gl::MAX_VERTEX_ATTRIBS];
        for (unsigned int j = 0; j < ilKey.elementCount; ++j)
        {
            descs[j] = ilKey.elements[j].desc;
        }

        HRESULT result = mDevice->CreateInputLayout(descs, ilKey.elementCount, shader11->getFunction(), shader11->getLength(), &inputLayout);
        if (FAILED(result))
        {
            return gl::Error(GL_OUT_OF_MEMORY, "Failed to create internal input layout, HRESULT: 0x%08x", result);
        }

        if (mInputLayoutMap.size() >= kMaxInputLayouts)
        {
            TRACE("Overflowed the limit of %u input layouts, removing the least recently used "
                  "to make room.", kMaxInputLayouts);

            InputLayoutMap::iterator leastRecentlyUsed = mInputLayoutMap.begin();
            for (InputLayoutMap::iterator i = mInputLayoutMap.begin(); i != mInputLayoutMap.end(); i++)
            {
                if (i->second.lastUsedTime < leastRecentlyUsed->second.lastUsedTime)
                {
                    leastRecentlyUsed = i;
                }
            }
            SafeRelease(leastRecentlyUsed->second.inputLayout);
            mInputLayoutMap.erase(leastRecentlyUsed);
        }

        InputLayoutCounterPair inputCounterPair;
        inputCounterPair.inputLayout = inputLayout;
        inputCounterPair.lastUsedTime = mCounter++;

        mInputLayoutMap.insert(std::make_pair(ilKey, inputCounterPair));
    }

    if (inputLayout != mCurrentIL)
    {
        mDeviceContext->IASetInputLayout(inputLayout);
        mCurrentIL = inputLayout;
    }

    bool dirtyBuffers = false;
    size_t minDiff = gl::MAX_VERTEX_ATTRIBS;
    size_t maxDiff = 0;
    for (unsigned int i = 0; i < gl::MAX_VERTEX_ATTRIBS; i++)
    {
        ID3D11Buffer *buffer = NULL;

        if (attributes[i].active)
        {
            VertexBuffer11 *vertexBuffer = VertexBuffer11::makeVertexBuffer11(attributes[i].vertexBuffer);
            Buffer11 *bufferStorage = attributes[i].storage ? Buffer11::makeBuffer11(attributes[i].storage) : NULL;

            buffer = bufferStorage ? bufferStorage->getBuffer(BUFFER_USAGE_VERTEX_OR_TRANSFORM_FEEDBACK)
                                   : vertexBuffer->getBuffer();
        }

        UINT vertexStride = attributes[i].stride;
        UINT vertexOffset = attributes[i].offset;

        if (buffer != mCurrentBuffers[i] || vertexStride != mCurrentVertexStrides[i] ||
            vertexOffset != mCurrentVertexOffsets[i])
        {
            dirtyBuffers = true;
            minDiff = std::min(minDiff, static_cast<size_t>(i));
            maxDiff = std::max(maxDiff, static_cast<size_t>(i));

            mCurrentBuffers[i] = buffer;
            mCurrentVertexStrides[i] = vertexStride;
            mCurrentVertexOffsets[i] = vertexOffset;
        }
    }

    if (moveFirstIndexedIntoSlotZero)
    {
        // In this case, we swapped the slots of the first instanced element and the first indexed element, to ensure
        // that the first slot contains non-instanced data (required by Feature Level 9_3).
        // We must also swap the corresponding buffers sent to IASetVertexBuffers so that the correct data is sent to each slot.
        std::swap(mCurrentBuffers[firstIndexedElement], mCurrentBuffers[firstInstancedElement]);
        std::swap(mCurrentVertexStrides[firstIndexedElement], mCurrentVertexStrides[firstInstancedElement]);
        std::swap(mCurrentVertexOffsets[firstIndexedElement], mCurrentVertexOffsets[firstInstancedElement]);
    }

    if (dirtyBuffers)
    {
        ASSERT(minDiff <= maxDiff && maxDiff < gl::MAX_VERTEX_ATTRIBS);
        mDeviceContext->IASetVertexBuffers(minDiff, maxDiff - minDiff + 1, mCurrentBuffers + minDiff,
                                           mCurrentVertexStrides + minDiff, mCurrentVertexOffsets + minDiff);
    }

    return gl::Error(GL_NO_ERROR);
}

std::size_t InputLayoutCache::hashInputLayout(const InputLayoutKey &inputLayout)
{
    static const unsigned int seed = 0xDEADBEEF;

    std::size_t hash = 0;
    MurmurHash3_x86_32(inputLayout.begin(), inputLayout.end() - inputLayout.begin(), seed, &hash);
    return hash;
}

bool InputLayoutCache::compareInputLayouts(const InputLayoutKey &a, const InputLayoutKey &b)
{
    if (a.elementCount != b.elementCount)
    {
        return false;
    }

    return std::equal(a.begin(), a.end(), b.begin());
}

}
