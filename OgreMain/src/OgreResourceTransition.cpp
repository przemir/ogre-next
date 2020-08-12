/*
-----------------------------------------------------------------------------
This source file is part of OGRE
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2014 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/

#include "OgreStableHeaders.h"

#include "OgreResourceTransition.h"

#include "OgreRenderSystem.h"
#include "OgreTextureGpu.h"
#include "OgreTextureGpuManager.h"

#include "ogrestd/map.h"

namespace Ogre
{
    namespace ResourceAccess
    {
        // clang-format off
        static const char* resourceAccessTable[] =
        {
            "Undefined",
            "Read",
            "Write",
            "ReadWrite"
        };
        // clang-format on

        const char *toString( ResourceAccess value ) { return resourceAccessTable[value]; }
    }  // namespace ResourceAccess

    GpuTrackedResource::~GpuTrackedResource() {}
    //-------------------------------------------------------------------------
    const ResourceStatusMap &BarrierSolver::getResourceStatus( void ) { return mResourceStatus; }
    //-------------------------------------------------------------------------
    void BarrierSolver::resetCopyLayoutsOnly( ResourceTransitionCollection &resourceTransitions )
    {
        FastArray<TextureGpu *>::const_iterator itor = mCopyStateTextures.begin();
        FastArray<TextureGpu *>::const_iterator endt = mCopyStateTextures.end();

        while( itor != endt )
        {
            TextureGpu *texture = *itor;
            ResourceLayout::Layout currLayout = texture->getCurrentLayout();
            if( currLayout == ResourceLayout::CopySrc || currLayout == ResourceLayout::CopyDst )
            {
                // It's still in copy layout. Transition the texture out of that.
                resolveTransition( resourceTransitions, texture, ResourceLayout::CopyEnd,
                                   ResourceAccess::Read, 0u );
            }
            ++itor;
        }

        mCopyStateTextures.clear();
    }
    //-------------------------------------------------------------------------
    void BarrierSolver::reset( ResourceTransitionCollection &resourceTransitions )
    {
        FastArray<TextureGpu *>::const_iterator itor = mCopyStateTextures.begin();
        FastArray<TextureGpu *>::const_iterator endt = mCopyStateTextures.end();

        while( itor != endt )
        {
            TextureGpu *texture = *itor;
            ResourceLayout::Layout currLayout = texture->getCurrentLayout();
            if( currLayout == ResourceLayout::CopySrc || currLayout == ResourceLayout::CopyDst )
            {
                // It's still in copy layout. Transition the texture out of that.
                resolveTransition( resourceTransitions, texture, ResourceLayout::CopyEnd,
                                   ResourceAccess::Read, 0u );
            }
            ++itor;
        }

        mCopyStateTextures.clear();
        mResourceStatus.clear();
    }
    //-------------------------------------------------------------------------
    void BarrierSolver::resolveTransition( ResourceTransitionCollection &resourceTransitions,
                                           TextureGpu *texture, ResourceLayout::Layout newLayout,
                                           ResourceAccess::ResourceAccess access, uint8 stageMask )
    {
        OGRE_ASSERT_MEDIUM( newLayout != ResourceLayout::Undefined );
        OGRE_ASSERT_MEDIUM( access != ResourceAccess::Undefined );

        OGRE_ASSERT_MEDIUM( ( newLayout == ResourceLayout::Texture || newLayout == ResourceLayout::Uav ||
                              stageMask == 0u ) &&
                            "stageMask must be 0 when layouts aren't Texture or Uav" );

        OGRE_ASSERT_MEDIUM(
            ( ( newLayout != ResourceLayout::Texture && newLayout != ResourceLayout::Uav ) ||
              ( newLayout == ResourceLayout::Texture && stageMask != 0u ) ||
              ( newLayout == ResourceLayout::Uav && stageMask != 0u ) ) &&
            "stageMask can't be 0 when layouts are Texture or Uav" );

        OGRE_ASSERT_MEDIUM(
            ( ( newLayout != ResourceLayout::Texture &&
                newLayout != ResourceLayout::RenderTargetReadOnly &&
                newLayout != ResourceLayout::CopySrc &&  //
                newLayout != ResourceLayout::CopyDst &&  //
                newLayout != ResourceLayout::MipmapGen ) ||
              ( newLayout == ResourceLayout::Texture && access == ResourceAccess::Read ) ||
              ( newLayout == ResourceLayout::CopySrc && access == ResourceAccess::Read ) ||
              ( newLayout == ResourceLayout::CopyDst && access == ResourceAccess::Write ) ||
              ( newLayout == ResourceLayout::MipmapGen && access == ResourceAccess::ReadWrite ) ||
              ( newLayout == ResourceLayout::RenderTargetReadOnly &&
                access == ResourceAccess::Read ) ) &&
            "Invalid Layout-access pair" );

        if( newLayout == ResourceLayout::CopySrc || newLayout == ResourceLayout::CopyDst )
        {
            // Keep track of textures which have been transitioned to Copy layouts, since
            // we can't finish the frame with textures in that stage as they're automatically
            // managed by the Copy Encoder.
            // Duplicate entries are harmless but we try to avoid it.
            if( mCopyStateTextures.empty() || mCopyStateTextures.back() != texture )
                mCopyStateTextures.push_back( texture );
        }

        ResourceStatusMap::iterator itor = mResourceStatus.find( texture );

        if( itor == mResourceStatus.end() )
        {
            ResourceStatus status;
            status.layout = newLayout;
            status.access = access;
            status.stageMask = stageMask;
            mResourceStatus.insert( ResourceStatusMap::value_type( texture, status ) );

            ResourceTransition resTrans;
            resTrans.resource = texture;
            if( texture->isDiscardableContent() )
            {
                resTrans.oldLayout = ResourceLayout::Undefined;
                if( access == ResourceAccess::Read )
                {
                    OGRE_EXCEPT(
                        Exception::ERR_INVALID_STATE,
                        "Transitioning texture " + texture->getNameStr() +
                            " from Undefined to a read-only layout. Perhaps you didn't want to set "
                            "TextureFlags::DiscardableContent / aka keep_content in compositor?",
                        "BarrierSolver::resolveTransition" );
                }
            }
            else
                resTrans.oldLayout = texture->getCurrentLayout();
            resTrans.oldAccess = ResourceAccess::Undefined;
            resTrans.newLayout = newLayout;
            resTrans.newAccess = access;
            resTrans.oldStageMask = 0;
            resTrans.newStageMask = stageMask;
            resourceTransitions.resourceTransitions.push_back( resTrans );
        }
        else
        {
            RenderSystem *renderSystem = texture->getTextureManager()->getRenderSystem();

            OGRE_ASSERT_LOW( renderSystem->isSameLayout( itor->second.layout,
                                                         texture->getCurrentLayout(), texture ) &&
                             "Layout was altered outside BarrierSolver!" );

            if( !renderSystem->isSameLayout( itor->second.layout, newLayout, texture ) ||
                ( newLayout == ResourceLayout::Uav &&  //
                  ( access != ResourceAccess::Read ||  //
                    itor->second.access != ResourceAccess::Read ) ) )
            {
                ResourceTransition resTrans;
                resTrans.resource = texture;
                resTrans.oldLayout = itor->second.layout;
                resTrans.newLayout = newLayout;
                resTrans.oldAccess = itor->second.access;
                resTrans.newAccess = access;
                resTrans.oldStageMask = itor->second.stageMask;
                resTrans.newStageMask = stageMask;

                resourceTransitions.resourceTransitions.push_back( resTrans );

                // After a barrier, the stageMask should be reset
                itor->second.stageMask = 0u;
            }

            itor->second.layout = newLayout;
            itor->second.access = access;
            itor->second.stageMask |= stageMask;
        }
    }
    //-------------------------------------------------------------------------
    void BarrierSolver::resolveTransition( ResourceTransitionCollection &resourceTransitions,
                                           GpuTrackedResource *bufferRes,
                                           ResourceAccess::ResourceAccess access, uint8 stageMask )
    {
        OGRE_ASSERT_MEDIUM( access != ResourceAccess::Undefined );

        ResourceStatusMap::iterator itor = mResourceStatus.find( bufferRes );

        if( itor == mResourceStatus.end() )
        {
            ResourceStatus status;
            status.layout = ResourceLayout::Undefined;
            status.access = access;
            status.stageMask = stageMask;
            mResourceStatus.insert( ResourceStatusMap::value_type( bufferRes, status ) );

            // No transition. There's nothing to wait for and unlike textures,
            // buffers have no layout to transition to
        }
        else
        {
            if( access != ResourceAccess::Read || itor->second.access != ResourceAccess::Read )
            {
                ResourceTransition resTrans;
                resTrans.resource = bufferRes;
                resTrans.oldLayout = ResourceLayout::Undefined;
                resTrans.newLayout = ResourceLayout::Undefined;
                resTrans.oldAccess = itor->second.access;
                resTrans.newAccess = access;
                resTrans.oldStageMask = itor->second.stageMask;
                resTrans.newStageMask = stageMask;

                resourceTransitions.resourceTransitions.push_back( resTrans );

                // After a barrier, the stageMask should be reset
                itor->second.stageMask = 0u;
            }

            itor->second.access = access;
            itor->second.stageMask |= stageMask;
        }
    }
    //-------------------------------------------------------------------------
    void BarrierSolver::assumeTransition( TextureGpu *texture, ResourceLayout::Layout newLayout,
                                          ResourceAccess::ResourceAccess access, uint8 stageMask )
    {
        OGRE_ASSERT_MEDIUM(
            ( ( newLayout != ResourceLayout::Texture &&
                newLayout != ResourceLayout::RenderTargetReadOnly &&
                newLayout != ResourceLayout::CopySrc &&  //
                newLayout != ResourceLayout::CopyDst &&  //
                newLayout != ResourceLayout::MipmapGen ) ||
              ( newLayout == ResourceLayout::Texture && access == ResourceAccess::Read ) ||
              ( newLayout == ResourceLayout::CopySrc && access == ResourceAccess::Read ) ||
              ( newLayout == ResourceLayout::CopyDst && access == ResourceAccess::Write ) ||
              ( newLayout == ResourceLayout::MipmapGen && access == ResourceAccess::ReadWrite ) ||
              ( newLayout == ResourceLayout::RenderTargetReadOnly &&
                access == ResourceAccess::Read ) ) &&
            "Invalid Layout-access pair" );

        ResourceStatus &status = mResourceStatus[texture];
        status.layout = newLayout;
        status.access = access;
        status.stageMask = stageMask;
    }
    //-------------------------------------------------------------------------
    void BarrierSolver::assumeTransitions( ResourceStatusMap &resourceStatus )
    {
        mResourceStatus.insert( resourceStatus.begin(), resourceStatus.end() );
    }
}  // namespace Ogre
